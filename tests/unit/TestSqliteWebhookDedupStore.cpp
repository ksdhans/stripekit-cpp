/** TestSqliteWebhookDedupStore.cpp
 *
 * Unit tests for SQLite-backed webhook de-duplication store.
 */

#include "StripeKit/Errors.h"
#include "StripeKit/SqliteWebhookDedupStore.h"
#include "TestFramework.h"

#include <chrono>
#include <filesystem>
#include <limits>
#include <memory>
#include <random>
#include <string>

#if defined(STRIPEKIT_WITH_SQLITE3)
#include <sqlite3.h>

namespace {

/**
 * Observes the store's actual connection without exposing it through the public API.
 *
 * SQLite calls auto-extensions when a connection opens, before store initialization.
 * The unit runner executes these tests serially.
 */
class ConnectionObserver {
public:
    ConnectionObserver() {
        connection = nullptr;
        EXPECT_EQ(sqlite3_auto_extension(reinterpret_cast<void (*)()>(capture)), SQLITE_OK);
    }

    ~ConnectionObserver() {
        sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(capture));
        connection = nullptr;
    }

    /** Reads a setting from the most recently opened connection on this thread. */
    std::string pragma(const char* sql) const {
        EXPECT_TRUE(connection != nullptr);
        sqlite3_stmt* raw_statement = nullptr;
        const int prepared = sqlite3_prepare_v2(connection, sql, -1, &raw_statement, nullptr);
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(
            raw_statement, sqlite3_finalize);
        EXPECT_EQ(prepared, SQLITE_OK);
        EXPECT_EQ(sqlite3_step(statement.get()), SQLITE_ROW);
        const auto* value = sqlite3_column_text(statement.get(), 0);
        EXPECT_TRUE(value != nullptr);
        return reinterpret_cast<const char*>(value);
    }

private:
    /** Records a newly opened connection; SQLite owns its lifetime. */
    static int capture(sqlite3* db, char**, const sqlite3_api_routines*) {
        connection = db;
        return SQLITE_OK;
    }

    inline static thread_local sqlite3* connection = nullptr;
};

} // namespace
#endif

using namespace stripekit;

class SqliteWebhookDedupStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        now = std::chrono::system_clock::from_time_t(1700000000);
#if defined(STRIPEKIT_WITH_SQLITE3)
        directory = std::filesystem::temp_directory_path() /
                    ("stripekit-sqlite-" + std::to_string(std::random_device{}()) + "-" +
                     std::to_string(std::random_device{}()));
        EXPECT_TRUE(std::filesystem::create_directory(directory));
        database_path = (directory / "webhooks.sqlite3").string();
#endif
    }

    void TearDown() override {
#if defined(STRIPEKIT_WITH_SQLITE3)
        std::filesystem::remove(database_path + "-wal");
        std::filesystem::remove(database_path + "-shm");
        std::filesystem::remove(database_path);
        std::filesystem::remove(directory);
#endif
    }

#if defined(STRIPEKIT_WITH_SQLITE3)
    std::filesystem::path directory;
    std::string database_path;
#endif
    std::chrono::system_clock::time_point now;
};

TEST_F(SqliteWebhookDedupStoreTest, AvailabilityCheck) {
#if defined(STRIPEKIT_WITH_SQLITE3)
    EXPECT_TRUE(sqlite_dedup_store_is_available());
#else
    EXPECT_FALSE(sqlite_dedup_store_is_available());
#endif
}

#if defined(STRIPEKIT_WITH_SQLITE3)
TEST_F(SqliteWebhookDedupStoreTest, DefaultFileSettingsAndPersistence) {
    ConnectionObserver observer;
    {
        SqliteWebhookDedupStore store(database_path);
        EXPECT_EQ(observer.pragma("PRAGMA journal_mode;"), "wal");
        EXPECT_EQ(observer.pragma("PRAGMA busy_timeout;"), "5000");
        EXPECT_EQ(observer.pragma("PRAGMA synchronous;"), "2");
        EXPECT_TRUE(std::stoi(observer.pragma("PRAGMA wal_autocheckpoint;")) > 0);
        EXPECT_EQ(store.claim({ .event_id = "evt_persist", .now = now }).outcome,
                  ClaimOutcome::Claimed);
        EXPECT_TRUE(store.mark_processed("evt_persist", now));
    }
    SqliteWebhookDedupStore reopened(database_path);
    EXPECT_EQ(observer.pragma("PRAGMA journal_mode;"), "wal");
    EXPECT_EQ(observer.pragma("PRAGMA busy_timeout;"), "5000");
    EXPECT_EQ(observer.pragma("PRAGMA synchronous;"), "2");
    EXPECT_EQ(reopened.claim({ .event_id = "evt_persist", .now = now }).outcome,
              ClaimOutcome::DuplicateProcessed);
}

TEST_F(SqliteWebhookDedupStoreTest, DisabledAutoCheckpointingIsReapplied) {
    ConnectionObserver observer;
    const SqliteWebhookDedupStoreOptions options{ .auto_checkpointing = false,
                                                  .busy_timeout = std::chrono::milliseconds(125),
                                                  .synchronous_mode =
                                                      SqliteSynchronousMode::Normal };
    for (int attempt = 0; attempt < 2; ++attempt) {
        SqliteWebhookDedupStore store(database_path, options);
        EXPECT_EQ(observer.pragma("PRAGMA journal_mode;"), "wal");
        EXPECT_EQ(observer.pragma("PRAGMA busy_timeout;"), "125");
        EXPECT_EQ(observer.pragma("PRAGMA synchronous;"), "1");
        EXPECT_EQ(observer.pragma("PRAGMA wal_autocheckpoint;"), "0");
    }
    SqliteWebhookDedupStore with_auto_checkpointing(database_path);
    EXPECT_TRUE(std::stoi(observer.pragma("PRAGMA wal_autocheckpoint;")) > 0);
    EXPECT_EQ(observer.pragma("PRAGMA synchronous;"), "2");
}

TEST_F(SqliteWebhookDedupStoreTest, DisabledAutoCheckpointingDoesNotChangeDefaultDurability) {
    ConnectionObserver observer;
    SqliteWebhookDedupStore store(database_path, { .auto_checkpointing = false });
    EXPECT_EQ(observer.pragma("PRAGMA synchronous;"), "2");
    EXPECT_EQ(observer.pragma("PRAGMA wal_autocheckpoint;"), "0");
}

TEST_F(SqliteWebhookDedupStoreTest, ZeroTimeoutAndNormalDurabilityWithAutoCheckpointing) {
    ConnectionObserver observer;
    SqliteWebhookDedupStore store(database_path,
                                  { .busy_timeout = std::chrono::milliseconds(0),
                                    .synchronous_mode = SqliteSynchronousMode::Normal });
    EXPECT_EQ(observer.pragma("PRAGMA busy_timeout;"), "0");
    EXPECT_EQ(observer.pragma("PRAGMA synchronous;"), "1");
    EXPECT_TRUE(std::stoi(observer.pragma("PRAGMA wal_autocheckpoint;")) > 0);
}

TEST_F(SqliteWebhookDedupStoreTest, InMemorySettingsDoNotRequireWal) {
    ConnectionObserver observer;
    SqliteWebhookDedupStore store(":memory:", { .busy_timeout = std::chrono::milliseconds(42) });
    EXPECT_EQ(observer.pragma("PRAGMA journal_mode;"), "memory");
    EXPECT_EQ(observer.pragma("PRAGMA busy_timeout;"), "42");
    EXPECT_EQ(observer.pragma("PRAGMA synchronous;"), "2");
}

TEST_F(SqliteWebhookDedupStoreTest, TemporaryDatabaseDoesNotRequireWal) {
    ConnectionObserver observer;
    SqliteWebhookDedupStore store("");
    EXPECT_EQ(observer.pragma("PRAGMA journal_mode;"), "delete");
    EXPECT_EQ(store.claim({ .event_id = "evt_temporary", .now = now }).outcome,
              ClaimOutcome::Claimed);
}

TEST_F(SqliteWebhookDedupStoreTest, RejectsDisabledAutoCheckpointingForNonPersistentDatabases) {
    EXPECT_THROW((SqliteWebhookDedupStore(":memory:", { .auto_checkpointing = false })),
                 ConfigurationException);
    EXPECT_THROW((SqliteWebhookDedupStore("", { .auto_checkpointing = false })),
                 ConfigurationException);
}

TEST_F(SqliteWebhookDedupStoreTest, RejectsInvalidTimeouts) {
    EXPECT_THROW(
        (SqliteWebhookDedupStore(":memory:", { .busy_timeout = std::chrono::milliseconds(-1) })),
        ConfigurationException);
    const auto too_large =
        std::chrono::milliseconds(static_cast<long long>(std::numeric_limits<int>::max()) + 1);
    EXPECT_THROW((SqliteWebhookDedupStore(":memory:", { .busy_timeout = too_large })),
                 ConfigurationException);
}

TEST_F(SqliteWebhookDedupStoreTest, RejectsInvalidSynchronousMode) {
    EXPECT_THROW((SqliteWebhookDedupStore(
                     ":memory:", { .synchronous_mode = static_cast<SqliteSynchronousMode>(99) })),
                 ConfigurationException);
}

TEST_F(SqliteWebhookDedupStoreTest, BusyTimeoutWaitsForCompetingWriter) {
    SqliteWebhookDedupStore store(database_path,
                                  { .busy_timeout = std::chrono::milliseconds(125) });
    sqlite3* raw_connection = nullptr;
    const int opened = sqlite3_open(database_path.c_str(), &raw_connection);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> writer(raw_connection, sqlite3_close);
    EXPECT_EQ(opened, SQLITE_OK);
    EXPECT_EQ(sqlite3_exec(writer.get(), "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), SQLITE_OK);

    const auto started = std::chrono::steady_clock::now();
    const auto result = store.claim({ .event_id = "evt_busy", .now = now });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(result.outcome, ClaimOutcome::RetryableStoreError);
    EXPECT_TRUE(elapsed >= std::chrono::milliseconds(100));

    EXPECT_EQ(sqlite3_exec(writer.get(), "ROLLBACK;", nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_EQ(store.claim({ .event_id = "evt_busy", .now = now }).outcome, ClaimOutcome::Claimed);
}

TEST_F(SqliteWebhookDedupStoreTest, WalInitializationFailureIsReportedAndConnectionIsClosed) {
    sqlite3* raw_connection = nullptr;
    const int opened = sqlite3_open(database_path.c_str(), &raw_connection);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> writer(raw_connection, sqlite3_close);
    EXPECT_EQ(opened, SQLITE_OK);
    EXPECT_EQ(sqlite3_exec(writer.get(), "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_THROW(
        (SqliteWebhookDedupStore(database_path, { .busy_timeout = std::chrono::milliseconds(0) })),
        ConfigurationException);

    EXPECT_EQ(sqlite3_exec(writer.get(), "ROLLBACK;", nullptr, nullptr, nullptr), SQLITE_OK);
    SqliteWebhookDedupStore store(database_path);
    EXPECT_EQ(store.claim({ .event_id = "evt_after_failure", .now = now }).outcome,
              ClaimOutcome::Claimed);
}

TEST_F(SqliteWebhookDedupStoreTest, InMemoryClaimAndDedupFlow) {
    SqliteWebhookDedupStore store(":memory:");

    const ClaimRequest request{ .event_id = "evt_sqlite_1",
                                .now = now,
                                .stale_inflight_timeout = std::chrono::minutes(15) };

    // First claim
    const ClaimResult first_result = store.claim(request);
    EXPECT_EQ(first_result.outcome, ClaimOutcome::Claimed);
    EXPECT_TRUE(first_result.should_process());
    EXPECT_EQ(first_result.detail, "first-seen");

    // In-flight duplicate
    const ClaimResult inflight_result = store.claim(request);
    EXPECT_EQ(inflight_result.outcome, ClaimOutcome::DuplicateInflight);
    EXPECT_FALSE(inflight_result.should_process());
    EXPECT_EQ(inflight_result.detail, "already-claimed-inflight");

    // Mark processed
    EXPECT_TRUE(store.mark_processed("evt_sqlite_1", now + std::chrono::seconds(5)));

    // Duplicate processed
    const ClaimResult processed_result = store.claim(request);
    EXPECT_EQ(processed_result.outcome, ClaimOutcome::DuplicateProcessed);
    EXPECT_FALSE(processed_result.should_process());
    EXPECT_EQ(processed_result.detail, "already-processed");
}

TEST_F(SqliteWebhookDedupStoreTest, MarkProcessedUnknownEventReturnsFalse) {
    SqliteWebhookDedupStore store(":memory:");
    EXPECT_FALSE(store.mark_processed("evt_non_existent", now));
}

TEST_F(SqliteWebhookDedupStoreTest, StaleInflightIsReclaimed) {
    SqliteWebhookDedupStore store(":memory:");

    const ClaimRequest initial_request{ .event_id = "evt_sqlite_stale",
                                        .now = now,
                                        .stale_inflight_timeout = std::chrono::minutes(15) };

    EXPECT_EQ(store.claim(initial_request).outcome, ClaimOutcome::Claimed);

    // 14 min later: still in-flight
    const ClaimRequest check_inflight{ .event_id = "evt_sqlite_stale",
                                       .now = now + std::chrono::minutes(14),
                                       .stale_inflight_timeout = std::chrono::minutes(15) };
    EXPECT_EQ(store.claim(check_inflight).outcome, ClaimOutcome::DuplicateInflight);

    // 15 min later: reclaimed
    const ClaimRequest check_reclaimed{ .event_id = "evt_sqlite_stale",
                                        .now = now + std::chrono::minutes(15),
                                        .stale_inflight_timeout = std::chrono::minutes(15) };
    const ClaimResult reclaimed = store.claim(check_reclaimed);
    EXPECT_EQ(reclaimed.outcome, ClaimOutcome::Claimed);
    EXPECT_TRUE(reclaimed.should_process());
    EXPECT_EQ(reclaimed.detail, "reclaimed-stale-inflight");
}

TEST_F(SqliteWebhookDedupStoreTest, EraseExpiredPurgesOldEntries) {
    SqliteWebhookDedupStore store(":memory:");

    const ClaimRequest old_req{ .event_id = "evt_old",
                                .now = now,
                                .stale_inflight_timeout = std::chrono::minutes(15) };
    store.claim(old_req);
    store.mark_processed("evt_old", now);

    const ClaimRequest fresh_req{ .event_id = "evt_fresh",
                                  .now = now + std::chrono::hours(20),
                                  .stale_inflight_timeout = std::chrono::minutes(15) };
    store.claim(fresh_req);
    store.mark_processed("evt_fresh", now + std::chrono::hours(20));

    // Erase records older than 24 hours at now + 25 hours
    store.erase_expired(now + std::chrono::hours(25), std::chrono::hours(24));

    // Old event is deleted, so claiming succeeds as first-seen
    const ClaimResult reclaim_old =
        store.claim(ClaimRequest{ .event_id = "evt_old",
                                  .now = now + std::chrono::hours(25),
                                  .stale_inflight_timeout = std::chrono::minutes(15) });
    EXPECT_EQ(reclaim_old.outcome, ClaimOutcome::Claimed);
    EXPECT_EQ(reclaim_old.detail, "first-seen");

    // Fresh event is kept
    const ClaimResult check_fresh =
        store.claim(ClaimRequest{ .event_id = "evt_fresh",
                                  .now = now + std::chrono::hours(25),
                                  .stale_inflight_timeout = std::chrono::minutes(15) });
    EXPECT_EQ(check_fresh.outcome, ClaimOutcome::DuplicateProcessed);
}

TEST_F(SqliteWebhookDedupStoreTest, InvalidPathThrowsConfigurationException) {
    EXPECT_THROW(
        { SqliteWebhookDedupStore store("/nonexistent_directory_for_test/database.sqlite3"); },
        ConfigurationException);
}

TEST_F(SqliteWebhookDedupStoreTest, ReleaseOnlyMatchingUnfinishedClaims) {
    SqliteWebhookDedupStore store(":memory:");
    EXPECT_FALSE(store.release_claim("evt_missing", now));
    store.claim(ClaimRequest{ .event_id = "evt_release", .now = now });
    EXPECT_FALSE(store.release_claim("evt_release", now + std::chrono::seconds(1)));
    EXPECT_TRUE(store.release_claim("evt_release", now));
    EXPECT_EQ(store.claim(ClaimRequest{ .event_id = "evt_release", .now = now }).outcome,
              ClaimOutcome::Claimed);
    store.mark_processed("evt_release", now);
    EXPECT_FALSE(store.release_claim("evt_release", now));
    EXPECT_EQ(store.claim(ClaimRequest{ .event_id = "evt_release", .now = now }).outcome,
              ClaimOutcome::DuplicateProcessed);
}

TEST_F(SqliteWebhookDedupStoreTest, OldAttemptCannotReleaseReclaimedEvent) {
    SqliteWebhookDedupStore store(":memory:");
    store.claim(ClaimRequest{ .event_id = "evt_reclaimed", .now = now });
    const auto later = now + defaultStaleInflightTimeout;
    EXPECT_EQ(store.claim(ClaimRequest{ .event_id = "evt_reclaimed", .now = later }).outcome,
              ClaimOutcome::Claimed);
    EXPECT_FALSE(store.release_claim("evt_reclaimed", now));
    EXPECT_EQ(store.claim(ClaimRequest{ .event_id = "evt_reclaimed", .now = later }).outcome,
              ClaimOutcome::DuplicateInflight);
}
#else
TEST_F(SqliteWebhookDedupStoreTest, ConstructorsRejectUnavailableSqlite) {
    EXPECT_THROW((SqliteWebhookDedupStore(":memory:")), ConfigurationException);
    EXPECT_THROW((SqliteWebhookDedupStore(":memory:", { .auto_checkpointing = false })),
                 ConfigurationException);
}
#endif
