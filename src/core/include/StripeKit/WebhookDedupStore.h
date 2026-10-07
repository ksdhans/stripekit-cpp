/** WebhookDedupStore.h
 *
 * Webhook event-id tracking used to prevent processing the same event twice.
 *
 * @author Hans de Ruiter
 *
 * @license See LICENSE.md for details.
 */

#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace stripekit {

/**
 * @addtogroup webhooks
 * @{
 */

/** How long an unfinished claim is honoured before another process may take it. */
inline constexpr std::chrono::minutes defaultStaleInflightTimeout{ 15 };

/**
 * The result of trying to reserve an event for processing.
 */
enum class ClaimOutcome {
    Claimed,             /**< This worker reserved the event id and should process it now. */
    DuplicateProcessed,  /**< A previous delivery already completed processing. */
    DuplicateInflight,   /**< Another worker is already processing the same event. */
    RetryableStoreError, /**< The store failed temporarily and the caller should retry. */
    FatalStoreError      /**< The store failed in a non-recoverable way. */
};

/**
 * What to tell Stripe about a delivery.
 */
enum class WebhookResponseAction {
    AcknowledgeSuccess, /**< Answer 2xx. Stripe stops retrying this event. */
    RetryLater          /**< Answer 5xx. Stripe delivers the event again later. */
};

/**
 * Asks the store to reserve an event id.
 *
 * The store must hand out a reservation to exactly one caller. If the id is
 * already reserved or already finished, it reports a duplicate instead, which
 * is what keeps repeated Stripe deliveries from being processed twice.
 */
struct ClaimRequest {
    /** The Stripe event id to reserve. */
    std::string event_id;

    /** Current time, used for claim timestamps and staleness checks. */
    std::chrono::system_clock::time_point now;

    /** How old an unfinished claim must be before it can be taken over. */
    std::chrono::minutes stale_inflight_timeout = defaultStaleInflightTimeout;
};

/**
 * The store's answer to a ClaimRequest.
 */
struct ClaimResult {
    /** What happened. */
    ClaimOutcome outcome = ClaimOutcome::FatalStoreError;

    /** Optional free-form detail from the implementation, for diagnostics. */
    std::string detail;

    /**
     * Reports whether the caller won the reservation.
     *
     * @return True only for ClaimOutcome::Claimed.
     */
    bool should_process() const;
};

/**
 * Remembers which webhook events have been handled.
 *
 * Stripe delivers the same event more than once whenever it cannot confirm
 * receipt. This interface skips completed events and reserves unfinished events
 * for one worker at a time. Handlers must still be safe to retry after failure
 * or a crash.
 *
 * Which implementation to use depends on the deployment. Use
 * InMemoryWebhookDedupStore for tests, SqliteWebhookDedupStore for a single
 * host, and an implementation backed by the application's shared database when
 * several instances handle the same endpoint.
 *
 * Implementations must be thread-safe, and claim() must be atomic: two
 * instances racing on the same event id must produce exactly one
 * ClaimOutcome::Claimed. A read followed by a write is not sufficient; use the
 * database's conditional insert.
 */
class WebhookDedupStore {
public:
    /**
     * Destroys the store instance.
     */
    virtual ~WebhookDedupStore() = default;

    /**
     * Tries to reserve an event id for processing.
     *
     * @param request The event id to reserve, and the timing rules to apply.
     * @return Whether this caller won the reservation, and why not if it did
     *         not. Store failures are reported here rather than thrown.
     */
    virtual ClaimResult claim(const ClaimRequest& request) = 0;

    /**
     * Records that an event has been fully handled.
     *
     * Later deliveries of the same event then report
     * ClaimOutcome::DuplicateProcessed.
     *
     * @param event_id The event id that was handled.
     * @param processed_at When it finished.
     * @return True if a claim record was updated; false if there was nothing
     *         to update.
     */
    virtual bool mark_processed(std::string_view event_id,
                                std::chrono::system_clock::time_point processed_at) = 0;

    /**
     * Releases an unfinished claim after its handler has stopped.
     *
     * Delete only the unprocessed record whose claim time matches @p claimed_at,
     * so an older attempt cannot release a newer worker's reservation.
     *
     * @param event_id The event to allow another delivery to retry.
     * @param claimed_at The successful ClaimRequest::now for this attempt.
     * @return True if the matching claim was removed; false if it no longer exists.
     * @throws StripeKitException The store could not release the claim.
     */
    virtual bool release_claim(std::string_view event_id,
                               std::chrono::system_clock::time_point claimed_at) = 0;

    /**
     * Deletes records older than the retention period.
     *
     * Call this periodically to stop the store growing without limit. Keep the
     * retention comfortably longer than Stripe's retry window, which runs for
     * several days, or an old event could be processed a second time.
     *
     * @param now Current time.
     * @param retention How long a record must be kept.
     */
    virtual void erase_expired(std::chrono::system_clock::time_point now,
                               std::chrono::hours retention) = 0;
};

/**
 * A de-duplication store that keeps everything in memory.
 *
 * Suitable for tests and local development. It forgets everything when the
 * process exits and is not shared between processes, so it is not appropriate
 * for production.
 *
 * Safe to use from several threads.
 */
class InMemoryWebhookDedupStore final : public WebhookDedupStore {
public:
    /**
     * Creates an empty store.
     */
    InMemoryWebhookDedupStore();

    /**
     * Destroys the store instance.
     */
    ~InMemoryWebhookDedupStore() override;

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
     * @param retention How long a record must be kept.
     */
    void erase_expired(std::chrono::system_clock::time_point now,
                       std::chrono::hours retention) override;

private:
    struct Entry {
        bool processed = false;
        std::chrono::system_clock::time_point claimed_at;
        std::chrono::system_clock::time_point processed_at;
    };

    std::mutex mutex;
    std::unordered_map<std::string, Entry> records;
};

/**
 * Works out what to tell Stripe, given a claim outcome.
 *
 * Already-processed duplicates count as success. In-flight duplicates and
 * store failures ask Stripe to retry.
 *
 * @param outcome The result of a claim attempt.
 * @return WebhookResponseAction::AcknowledgeSuccess for a claim or processed
 *         duplicate; WebhookResponseAction::RetryLater otherwise.
 */
WebhookResponseAction map_claim_outcome_to_response(ClaimOutcome outcome);

/** @} */

} // namespace stripekit
