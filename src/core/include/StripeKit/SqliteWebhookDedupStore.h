/** SqliteWebhookDedupStore.h
 *
 * Optional SQLite-backed implementation of webhook event-id tracking.
 * It stores claim and processed state in a SQLite database.
 *
 * @author Hans de Ruiter
 *
 * @License See LICENSE for details.
 */

#pragma once

#include "StripeKit/WebhookDedupStore.h"

#include <chrono>
#include <mutex>
#include <string>

struct sqlite3;

namespace stripekit {

/** @ingroup webhooks
 * Durability of committed SQLite webhook records.
 */
enum class SqliteSynchronousMode {
    Full,  /**< Sync every commit to disk, subject to the storage system's guarantees. */
    Normal /**< Faster WAL commits; a power failure may lose recent committed records. */
};

/** @ingroup webhooks
 * Settings applied whenever a SQLite webhook store opens a connection.
 */
struct SqliteWebhookDedupStoreOptions {
    /**
     * Whether SQLite automatically checkpoints the WAL.
     *
     * Disable only when an external checkpoint manager, such as Litestream,
     * is configured and running, or the WAL may grow without limit. Disabling
     * requires a file-backed database and does not change commit durability.
     */
    bool auto_checkpointing = true;

    /**
     * How long SQLite may wait for a database lock before reporting failure.
     *
     * Zero disables waiting. Must fit in a non-negative int number of milliseconds.
     * Store operations are synchronous, so this wait blocks the calling thread.
     */
    std::chrono::milliseconds busy_timeout{ 5000 };

    /** Commit durability. Normal allows recent records to be lost on power failure. */
    SqliteSynchronousMode synchronous_mode = SqliteSynchronousMode::Full;
};

/**
 * @ingroup webhooks
 *
 * A de-duplication store that uses a SQLite database.
 *
 * The sensible default for an application running on a single host: unlike
 * InMemoryWebhookDedupStore it survives restarts, and it needs no database
 * server. Applications running several instances should implement
 * WebhookDedupStore against their shared database instead, since separate
 * SQLite files cannot agree on which events are done.
 *
 * Only available when StripeKit was built with SQLite support; check
 * sqlite_dedup_store_is_available() if that is in doubt.
 *
 * @code
 * static stripekit::SqliteWebhookDedupStore dedup_store("stripe-webhooks.sqlite3");
 * @endcode
 *
 * The store must outlive every handler that uses it. Safe to use from several
 * threads.
 */
class SqliteWebhookDedupStore final : public WebhookDedupStore {
public:
    /**
     * Opens or creates the database.
     *
     * Uses default SqliteWebhookDedupStoreOptions. Missing tables are created.
     * File-backed databases use WAL mode; in-memory and temporary databases
     * retain their journal mode.
     *
     * @param database_path Path to the SQLite file. Its directory must exist
     *        and be writable, and the file must be on local storage; network
     *        filesystems do not give SQLite the locking it needs.
     * @throws ConfigurationException The database could not be opened or
     *         prepared, or this build has no SQLite support.
     */
    explicit SqliteWebhookDedupStore(std::string database_path);

    /**
     * Opens or creates the database with explicit connection settings.
     *
     * File-backed databases use WAL mode. Automatic checkpoints remain enabled
     * unless options.auto_checkpointing is false. Settings are applied and checked
     * before the store can be used.
     *
     * @param database_path Local database path, ":memory:", or an empty string
     *        for a temporary database. The parent directory must exist and be writable.
     * @param options Lock timeout, durability, and automatic checkpoint settings.
     * @throws ConfigurationException Options are invalid, the database cannot be
     *         opened or configured, or SQLite support is unavailable.
     */
    SqliteWebhookDedupStore(std::string database_path, SqliteWebhookDedupStoreOptions options);

    /**
     * Closes the database and destroys the store.
     */
    ~SqliteWebhookDedupStore() override;

    /**
     * Tries to reserve an event id for processing.
     *
     * @param request The event id to reserve, and the timing rules to apply.
     * @return Whether this caller won the reservation.
     */
    ClaimResult claim(const ClaimRequest& request) override;

    /**
     * Records that an event has been fully handled.
     *
     * @param event_id The event id that was handled.
     * @param processed_at When it finished.
     * @return True if a claim record was updated.
     */
    bool mark_processed(std::string_view event_id,
                        std::chrono::system_clock::time_point processed_at) override;

    /** @copydoc WebhookDedupStore::release_claim */
    bool release_claim(std::string_view event_id,
                       std::chrono::system_clock::time_point claimed_at) override;

    /**
     * Deletes records older than the retention period.
     *
     * @param now Current time.
     * @param retention How long a record must be kept. Keep this longer than
     *        Stripe's retry window, which runs for several days.
     */
    void erase_expired(std::chrono::system_clock::time_point now,
                       std::chrono::hours retention) override;

private:
    sqlite3* db = nullptr;
    std::mutex mutex;
};

/**
 * @ingroup webhooks
 *
 * Reports whether this build can use SqliteWebhookDedupStore.
 *
 * SQLite support is a build option, so a StripeKit built without it will fail
 * to construct the store.
 *
 * @return True when SQLite support was compiled in.
 */
bool sqlite_dedup_store_is_available();

} // namespace stripekit
