/** SqliteWebhookDedupStore.h
 * 
 * Optional SQLite-backed implementation of webhook event-id tracking.
 * It stores claim and processed state in a SQLite database.
 * 
 * @author Hans de Ruiter
 * 
 * @license See LICENSE.md for details.
 */

#pragma once

#include "StripeKit/WebhookDedupStore.h"

#include <mutex>
#include <string>

struct sqlite3;

namespace stripekit {

/**
 * Optional SQLite-backed de-duplication store implementation.
 */
class SqliteWebhookDedupStore final : public WebhookDedupStore {
public:
    /**
     * Creates a SQLite-backed store using the provided database path.
     */
    explicit SqliteWebhookDedupStore(std::string database_path);

    /**
     * Destroys the store and closes database resources.
     */
    ~SqliteWebhookDedupStore() override;

    /**
     * Attempts to claim an event id for processing.
     */
    ClaimResult claim(const ClaimRequest& request) override;

    /**
     * Marks an event id as processed.
     */
    bool mark_processed(std::string_view event_id,
                        std::chrono::system_clock::time_point processed_at) override;

    /**
     * Removes expired records according to retention policy.
     */
    void erase_expired(std::chrono::system_clock::time_point now,
                       std::chrono::hours retention) override;

private:
    sqlite3* db = nullptr;
    std::mutex mutex;
};

/**
 * Returns true when SQLite support is compiled into this build.
 */
bool sqlite_dedup_store_is_available();

} // namespace stripekit
