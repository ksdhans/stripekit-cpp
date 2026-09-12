/** SqliteWebhookDedupStore.cpp
 *
 * See header file for details.
 */

#include "StripeKit/SqliteWebhookDedupStore.h"

#include "StripeKit/Errors.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#if defined(STRIPEKIT_WITH_SQLITE3)
#include <sqlite3.h>
#endif

namespace stripekit {

SqliteWebhookDedupStore::SqliteWebhookDedupStore(std::string database_path) {
#if defined(STRIPEKIT_WITH_SQLITE3)
    if (sqlite3_open(database_path.c_str(), &db) != SQLITE_OK) {
        const std::string message = db != nullptr ? sqlite3_errmsg(db) : "unknown sqlite open error";
        if (db != nullptr) {
            sqlite3_close(db);
            db = nullptr;
        }
        throw ConfigurationException("Failed to open SQLite database: " + message);
    }

    const char* schema_sql =
        "CREATE TABLE IF NOT EXISTS stripekit_webhook_dedup ("
        "  event_id TEXT PRIMARY KEY,"
        "  processed INTEGER NOT NULL,"
        "  claimed_at_unix INTEGER NOT NULL,"
        "  processed_at_unix INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS stripekit_webhook_dedup_processed_idx "
        "ON stripekit_webhook_dedup(processed, processed_at_unix);";

    char* error_message = nullptr;
    if (sqlite3_exec(db, schema_sql, nullptr, nullptr, &error_message) != SQLITE_OK) {
        const std::string message = error_message != nullptr ? error_message : "unknown sqlite schema error";
        if (error_message != nullptr) {
            sqlite3_free(error_message);
        }
        sqlite3_close(db);
        db = nullptr;
        throw ConfigurationException("Failed to initialize SQLite schema: " + message);
    }
#else
    (void)database_path;
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
    const char* select_sql =
        "SELECT processed, claimed_at_unix, processed_at_unix "
        "FROM stripekit_webhook_dedup WHERE event_id = ?1;";
    if (sqlite3_prepare_v2(db, select_sql, -1, &select_stmt, nullptr) != SQLITE_OK) {
        return ClaimResult{.outcome = ClaimOutcome::RetryableStoreError, .detail = sqlite3_errmsg(db)};
    }

    sqlite3_bind_text(select_stmt, 1, request.event_id.c_str(), -1, SQLITE_TRANSIENT);
    const int select_rc = sqlite3_step(select_stmt);

    const auto now_unix = std::chrono::duration_cast<std::chrono::seconds>(
        request.now.time_since_epoch()).count();

    if (select_rc == SQLITE_DONE) {
        sqlite3_finalize(select_stmt);

        sqlite3_stmt* insert_stmt = nullptr;
        const char* insert_sql =
            "INSERT INTO stripekit_webhook_dedup (event_id, processed, claimed_at_unix, processed_at_unix) "
            "VALUES (?1, 0, ?2, 0);";
        if (sqlite3_prepare_v2(db, insert_sql, -1, &insert_stmt, nullptr) != SQLITE_OK) {
            return ClaimResult{.outcome = ClaimOutcome::RetryableStoreError, .detail = sqlite3_errmsg(db)};
        }

        sqlite3_bind_text(insert_stmt, 1, request.event_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_stmt, 2, static_cast<sqlite3_int64>(now_unix));

        const int insert_rc = sqlite3_step(insert_stmt);
        sqlite3_finalize(insert_stmt);
        if (insert_rc != SQLITE_DONE) {
            return ClaimResult{.outcome = ClaimOutcome::RetryableStoreError, .detail = sqlite3_errmsg(db)};
        }

        return ClaimResult{.outcome = ClaimOutcome::Claimed, .detail = "first-seen"};
    }

    if (select_rc != SQLITE_ROW) {
        sqlite3_finalize(select_stmt);
        return ClaimResult{.outcome = ClaimOutcome::RetryableStoreError, .detail = sqlite3_errmsg(db)};
    }

    const bool processed = sqlite3_column_int(select_stmt, 0) != 0;
    const auto claimed_at_unix = static_cast<std::int64_t>(sqlite3_column_int64(select_stmt, 1));
    sqlite3_finalize(select_stmt);

    if (processed) {
        return ClaimResult{.outcome = ClaimOutcome::DuplicateProcessed, .detail = "already-processed"};
    }

    const auto age_seconds = now_unix - claimed_at_unix;
    if (age_seconds >= std::chrono::duration_cast<std::chrono::seconds>(request.stale_inflight_timeout).count()) {
        sqlite3_stmt* update_stmt = nullptr;
        const char* update_sql =
            "UPDATE stripekit_webhook_dedup SET claimed_at_unix = ?1 "
            "WHERE event_id = ?2 AND processed = 0 AND claimed_at_unix = ?3;";
        if (sqlite3_prepare_v2(db, update_sql, -1, &update_stmt, nullptr) != SQLITE_OK) {
            return ClaimResult{.outcome = ClaimOutcome::RetryableStoreError, .detail = sqlite3_errmsg(db)};
        }

        sqlite3_bind_int64(update_stmt, 1, static_cast<sqlite3_int64>(now_unix));
        sqlite3_bind_text(update_stmt, 2, request.event_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(update_stmt, 3, static_cast<sqlite3_int64>(claimed_at_unix));

        const int update_rc = sqlite3_step(update_stmt);
        const int changed = sqlite3_changes(db);
        sqlite3_finalize(update_stmt);
        if (update_rc != SQLITE_DONE) {
            return ClaimResult{.outcome = ClaimOutcome::RetryableStoreError, .detail = sqlite3_errmsg(db)};
        }

        if (changed == 0) {
            return ClaimResult{.outcome = ClaimOutcome::DuplicateInflight, .detail = "already-claimed-inflight"};
        }

        return ClaimResult{.outcome = ClaimOutcome::Claimed, .detail = "reclaimed-stale-inflight"};
    }

    return ClaimResult{.outcome = ClaimOutcome::DuplicateInflight, .detail = "already-claimed-inflight"};
#else
    (void)request;
    return ClaimResult{.outcome = ClaimOutcome::FatalStoreError, .detail = "sqlite unavailable"};
#endif
}

bool SqliteWebhookDedupStore::mark_processed(
    std::string_view event_id,
    std::chrono::system_clock::time_point processed_at) {
#if defined(STRIPEKIT_WITH_SQLITE3)
    std::lock_guard<std::mutex> lock(mutex);

    sqlite3_stmt* update_stmt = nullptr;
    const char* update_sql =
        "UPDATE stripekit_webhook_dedup "
        "SET processed = 1, processed_at_unix = ?1 "
        "WHERE event_id = ?2;";
    if (sqlite3_prepare_v2(db, update_sql, -1, &update_stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    const auto processed_unix = std::chrono::duration_cast<std::chrono::seconds>(
        processed_at.time_since_epoch()).count();

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

void SqliteWebhookDedupStore::erase_expired(
    std::chrono::system_clock::time_point now,
    std::chrono::hours retention) {
#if defined(STRIPEKIT_WITH_SQLITE3)
    std::lock_guard<std::mutex> lock(mutex);

    const auto threshold_unix = std::chrono::duration_cast<std::chrono::seconds>(
        (now - retention).time_since_epoch()).count();

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