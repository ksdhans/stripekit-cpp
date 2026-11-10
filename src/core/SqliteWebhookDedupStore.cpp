/** SqliteWebhookDedupStore.cpp
 *
 * See header file for details.
 */

#include "StripeKit/SqliteWebhookDedupStore.h"

#include "StripeKit/Errors.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#if defined(STRIPEKIT_WITH_SQLITE3)
#include <sqlite3.h>
#endif

namespace stripekit {

#if defined(STRIPEKIT_WITH_SQLITE3)
namespace {

/** Runs an initialization command, reporting SQLite failures to the caller. */
void execute_initialization(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw ConfigurationException(std::string("Failed SQLite initialization (") + sql +
                                     "): " + sqlite3_errmsg(db));
    }
}

/** Reads a single PRAGMA value so requested settings can be checked. */
std::string read_pragma(sqlite3* db, const char* sql) {
    sqlite3_stmt* raw_statement = nullptr;
    const int prepared = sqlite3_prepare_v2(db, sql, -1, &raw_statement, nullptr);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw_statement,
                                                                               sqlite3_finalize);
    if (prepared != SQLITE_OK || sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw ConfigurationException(std::string("Failed to read SQLite setting (") + sql +
                                     "): " + sqlite3_errmsg(db));
    }
    const auto* value = sqlite3_column_text(statement.get(), 0);
    if (value == nullptr) {
        throw ConfigurationException(std::string("Missing SQLite setting value: ") + sql);
    }
    return reinterpret_cast<const char*>(value);
}

/** Rejects initialization when SQLite did not retain a requested setting. */
void require_pragma(sqlite3* db, const char* sql, const std::string& expected) {
    const auto actual = read_pragma(db, sql);
    if (actual != expected) {
        throw ConfigurationException(std::string("Unexpected SQLite setting (") + sql +
                                     "): expected " + expected + ", got " + actual);
    }
}

} // namespace
#endif

SqliteWebhookDedupStore::SqliteWebhookDedupStore(std::string database_path)
    : SqliteWebhookDedupStore(std::move(database_path), {}) {}

SqliteWebhookDedupStore::SqliteWebhookDedupStore(std::string database_path,
                                                 SqliteWebhookDedupStoreOptions options) {
#if defined(STRIPEKIT_WITH_SQLITE3)
    if (options.busy_timeout.count() < 0 ||
        options.busy_timeout.count() > std::numeric_limits<int>::max()) {
        throw ConfigurationException("SQLite busy_timeout must fit in a non-negative int");
    }
    if (options.synchronous_mode != SqliteSynchronousMode::Full &&
        options.synchronous_mode != SqliteSynchronousMode::Normal) {
        throw ConfigurationException("Invalid SQLite synchronous_mode");
    }

    sqlite3* raw_connection = nullptr;
    const int opened = sqlite3_open(database_path.c_str(), &raw_connection);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(raw_connection, sqlite3_close);
    if (opened != SQLITE_OK) {
        const std::string message =
            connection != nullptr ? sqlite3_errmsg(connection.get()) : "unknown sqlite open error";
        throw ConfigurationException("Failed to open SQLite database: " + message);
    }

    if (sqlite3_busy_timeout(connection.get(), static_cast<int>(options.busy_timeout.count())) !=
        SQLITE_OK) {
        throw ConfigurationException("Failed to set SQLite busy_timeout: " +
                                     std::string(sqlite3_errmsg(connection.get())));
    }
    require_pragma(
        connection.get(), "PRAGMA busy_timeout;", std::to_string(options.busy_timeout.count()));

    const char* filename = sqlite3_db_filename(connection.get(), "main");
    const bool file_backed = filename != nullptr && *filename != '\0';
    if (!options.auto_checkpointing && !file_backed) {
        throw ConfigurationException(
            "Disabling automatic checkpoints requires a file-backed SQLite database");
    }
    if (file_backed) {
        if (read_pragma(connection.get(), "PRAGMA journal_mode;") != "wal") {
            execute_initialization(connection.get(), "PRAGMA journal_mode=WAL;");
        }
        require_pragma(connection.get(), "PRAGMA journal_mode;", "wal");
    }

    const bool full = options.synchronous_mode == SqliteSynchronousMode::Full;
    execute_initialization(connection.get(),
                           full ? "PRAGMA synchronous=FULL;" : "PRAGMA synchronous=NORMAL;");
    require_pragma(connection.get(), "PRAGMA synchronous;", full ? "2" : "1");
    if (!options.auto_checkpointing) {
        execute_initialization(connection.get(), "PRAGMA wal_autocheckpoint=0;");
        require_pragma(connection.get(), "PRAGMA wal_autocheckpoint;", "0");
    }

    const char* schema_sql = "CREATE TABLE IF NOT EXISTS stripekit_webhook_dedup ("
                             "  event_id TEXT PRIMARY KEY,"
                             "  processed INTEGER NOT NULL,"
                             "  claimed_at_unix INTEGER NOT NULL,"
                             "  processed_at_unix INTEGER NOT NULL"
                             ");"
                             "CREATE INDEX IF NOT EXISTS stripekit_webhook_dedup_processed_idx "
                             "ON stripekit_webhook_dedup(processed, processed_at_unix);";

    execute_initialization(connection.get(), schema_sql);
    db = connection.release();
#else
    (void)database_path;
    (void)options;
    throw ConfigurationException("StripeKit was built without SQLite support");
#endif
}

SqliteWebhookDedupStore::~SqliteWebhookDedupStore() {
#if defined(STRIPEKIT_WITH_SQLITE3)
    if (db != nullptr) {
        sqlite3_close(db);
        db = nullptr;
    }
#endif
}

ClaimResult SqliteWebhookDedupStore::claim(const ClaimRequest& request) {
#if defined(STRIPEKIT_WITH_SQLITE3)
    std::lock_guard<std::mutex> lock(mutex);

    sqlite3_stmt* select_stmt = nullptr;
    const char* select_sql = "SELECT processed, claimed_at_unix, processed_at_unix "
                             "FROM stripekit_webhook_dedup WHERE event_id = ?1;";
    if (sqlite3_prepare_v2(db, select_sql, -1, &select_stmt, nullptr) != SQLITE_OK) {
        return ClaimResult{ .outcome = ClaimOutcome::RetryableStoreError,
                            .detail = sqlite3_errmsg(db) };
    }

    sqlite3_bind_text(select_stmt, 1, request.event_id.c_str(), -1, SQLITE_TRANSIENT);
    const int select_rc = sqlite3_step(select_stmt);

    const auto now_unix =
        std::chrono::duration_cast<std::chrono::seconds>(request.now.time_since_epoch()).count();

    if (select_rc == SQLITE_DONE) {
        sqlite3_finalize(select_stmt);

        sqlite3_stmt* insert_stmt = nullptr;
        const char* insert_sql = "INSERT INTO stripekit_webhook_dedup (event_id, processed, "
                                 "claimed_at_unix, processed_at_unix) "
                                 "VALUES (?1, 0, ?2, 0);";
        if (sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, nullptr) != SQLITE_OK) {
            return ClaimResult{ .outcome = ClaimOutcome::RetryableStoreError,
                                .detail = sqlite3_errmsg(db) };
        }

        sqlite3_bind_text(insert_stmt, 1, request.event_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_stmt, 2, static_cast<sqlite3_int64>(now_unix));

        const int insert_rc = sqlite3_step(insert_stmt);
        sqlite3_finalize(insert_stmt);
        if (insert_rc != SQLITE_DONE) {
            return ClaimResult{ .outcome = ClaimOutcome::RetryableStoreError,
                                .detail = sqlite3_errmsg(db) };
        }

        return ClaimResult{ .outcome = ClaimOutcome::Claimed, .detail = "first-seen" };
    }

    if (select_rc != SQLITE_ROW) {
        sqlite3_finalize(select_stmt);
        return ClaimResult{ .outcome = ClaimOutcome::RetryableStoreError,
                            .detail = sqlite3_errmsg(db) };
    }

    const bool processed = sqlite3_column_int(select_stmt, 0) != 0;
    const auto claimed_at_unix = static_cast<std::int64_t>(sqlite3_column_int64(select_stmt, 1));
    sqlite3_finalize(select_stmt);

    if (processed) {
        return ClaimResult{ .outcome = ClaimOutcome::DuplicateProcessed,
                            .detail = "already-processed" };
    }

    const auto age_seconds = now_unix - claimed_at_unix;
    if (age_seconds >=
        std::chrono::duration_cast<std::chrono::seconds>(request.stale_inflight_timeout).count()) {
        sqlite3_stmt* update_stmt = nullptr;
        const char* update_sql = "UPDATE stripekit_webhook_dedup SET claimed_at_unix = ?1 "
                                 "WHERE event_id = ?2 AND processed = 0 AND claimed_at_unix = ?3;";
        if (sqlite3_prepare_v2(db, update_sql, -1, &update_stmt, nullptr) != SQLITE_OK) {
            return ClaimResult{ .outcome = ClaimOutcome::RetryableStoreError,
                                .detail = sqlite3_errmsg(db) };
        }

        sqlite3_bind_int64(update_stmt, 1, static_cast<sqlite3_int64>(now_unix));
        sqlite3_bind_text(update_stmt, 2, request.event_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(update_stmt, 3, static_cast<sqlite3_int64>(claimed_at_unix));

        const int update_rc = sqlite3_step(update_stmt);
        const int changed = sqlite3_changes(db);
        sqlite3_finalize(update_stmt);
        if (update_rc != SQLITE_DONE) {
            return ClaimResult{ .outcome = ClaimOutcome::RetryableStoreError,
                                .detail = sqlite3_errmsg(db) };
        }

        if (changed == 0) {
            return ClaimResult{ .outcome = ClaimOutcome::DuplicateInflight,
                                .detail = "already-claimed-inflight" };
        }

        return ClaimResult{ .outcome = ClaimOutcome::Claimed,
                            .detail = "reclaimed-stale-inflight" };
    }

    return ClaimResult{ .outcome = ClaimOutcome::DuplicateInflight,
                        .detail = "already-claimed-inflight" };
#else
    (void)request;
    return ClaimResult{ .outcome = ClaimOutcome::FatalStoreError, .detail = "sqlite unavailable" };
#endif
}

bool SqliteWebhookDedupStore::mark_processed(std::string_view event_id,
                                             std::chrono::system_clock::time_point processed_at) {
#if defined(STRIPEKIT_WITH_SQLITE3)
    std::lock_guard<std::mutex> lock(mutex);

    sqlite3_stmt* update_stmt = nullptr;
    const char* update_sql = "UPDATE stripekit_webhook_dedup "
                             "SET processed = 1, processed_at_unix = ?1 "
                             "WHERE event_id = ?2;";
    if (sqlite3_prepare_v2(db, update_sql, -1, &update_stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    const auto processed_unix =
        std::chrono::duration_cast<std::chrono::seconds>(processed_at.time_since_epoch()).count();

    sqlite3_bind_int64(update_stmt, 1, static_cast<sqlite3_int64>(processed_unix));
    const std::string event_id_copy(event_id);
    sqlite3_bind_text(update_stmt, 2, event_id_copy.c_str(), -1, SQLITE_TRANSIENT);

    const int update_rc = sqlite3_step(update_stmt);
    const int changed = sqlite3_changes(db);
    sqlite3_finalize(update_stmt);

    return update_rc == SQLITE_DONE && changed > 0;
#else
    (void)event_id;
    (void)processed_at;
    return false;
#endif
}

bool SqliteWebhookDedupStore::release_claim(std::string_view event_id,
                                            std::chrono::system_clock::time_point claimed_at) {
#if defined(STRIPEKIT_WITH_SQLITE3)
    std::lock_guard<std::mutex> lock(mutex);
    sqlite3_stmt* statement = nullptr;
    const char* sql = "DELETE FROM stripekit_webhook_dedup "
                      "WHERE event_id = ?1 AND processed = 0 AND claimed_at_unix = ?2;";
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK) {
        throw ConfigurationException("Failed to release webhook claim: " +
                                     std::string(sqlite3_errmsg(db)));
    }
    const std::string id(event_id);
    const auto claimed_unix =
        std::chrono::duration_cast<std::chrono::seconds>(claimed_at.time_since_epoch()).count();
    sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(claimed_unix));
    const int result = sqlite3_step(statement);
    const int changed = sqlite3_changes(db);
    const std::string error = result != SQLITE_DONE ? sqlite3_errmsg(db) : "";
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        throw ConfigurationException("Failed to release webhook claim: " + error);
    }
    return changed > 0;
#else
    (void)event_id;
    (void)claimed_at;
    throw ConfigurationException("StripeKit was built without SQLite support");
#endif
}

void SqliteWebhookDedupStore::erase_expired(std::chrono::system_clock::time_point now,
                                            std::chrono::hours retention) {
#if defined(STRIPEKIT_WITH_SQLITE3)
    std::lock_guard<std::mutex> lock(mutex);

    const auto threshold_unix =
        std::chrono::duration_cast<std::chrono::seconds>((now - retention).time_since_epoch())
            .count();

    sqlite3_stmt* delete_stmt = nullptr;
    const char* delete_sql =
        "DELETE FROM stripekit_webhook_dedup "
        "WHERE (processed = 1 AND processed_at_unix > 0 AND processed_at_unix < ?1) "
        "   OR (processed = 0 AND claimed_at_unix < ?1);";
    if (sqlite3_prepare_v2(db, delete_sql, -1, &delete_stmt, nullptr) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_int64(delete_stmt, 1, static_cast<sqlite3_int64>(threshold_unix));
    (void)sqlite3_step(delete_stmt);
    sqlite3_finalize(delete_stmt);
#else
    (void)now;
    (void)retention;
#endif
}

bool sqlite_dedup_store_is_available() {
#if defined(STRIPEKIT_WITH_SQLITE3)
    return true;
#else
    return false;
#endif
}

} // namespace stripekit