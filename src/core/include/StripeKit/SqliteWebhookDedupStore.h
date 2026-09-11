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
     * Missing tables are created, so a fresh path just works.
     *
     * @param database_path Path to the SQLite file. Its directory must exist
     *        and be writable, and the file must be on local storage; network
     *        filesystems do not give SQLite the locking it needs.
     * @throws ConfigurationException The database could not be opened or
     *         prepared, or this build has no SQLite support.
     */
    explicit SqliteWebhookDedupStore(std::string database_path);

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
