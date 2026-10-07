/** TestSqliteWebhookDedupStore.cpp
 *
 * Unit tests for SQLite-backed webhook de-duplication store.
 */

#include "StripeKit/Errors.h"
#include "StripeKit/SqliteWebhookDedupStore.h"
#include "TestFramework.h"

#include <chrono>
#include <string>

using namespace stripekit;

class SqliteWebhookDedupStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        now = std::chrono::system_clock::from_time_t(1700000000);
    }

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
#endif
