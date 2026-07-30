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

/** Default timeout used to reclaim stale in-flight webhook claims. */
inline constexpr std::chrono::minutes defaultStaleInflightTimeout{15};

/**
 * Possible results when claiming a webhook event id for processing.
 */
enum class ClaimOutcome {
    Claimed, /**< This worker reserved the event id and should process it now. */
    DuplicateProcessed, /**< A previous delivery already completed processing. */
    DuplicateInflight, /**< Another worker is already processing the same event. */
    RetryableStoreError, /**< The store failed temporarily and the caller should retry. */
    FatalStoreError /**< The store failed in a non-recoverable way. */
};

/**
 * Default response action mapping for claim outcomes.
 */
enum class WebhookResponseAction {
    AcknowledgeSuccess,
    RetryLater
};

/**
 * Input passed when claiming an event id in the store.
 *
 * A claim request asks the store to reserve the Stripe event id for exactly one
 * processor. If the id is already reserved or already processed, the store
 * returns a duplicate outcome instead of allowing a second processor to work it.
 */
struct ClaimRequest {
    /** Stripe event id being reserved for processing. */
    std::string event_id;

    /** Current wall-clock time used by store implementations. */
    std::chrono::system_clock::time_point now;

    /** Reclaim timeout for stale in-flight claim records. */
    std::chrono::minutes stale_inflight_timeout = defaultStaleInflightTimeout;
};

/**
 * Result returned by claim operations.
 */
struct ClaimResult {
    /** Store-level claim outcome category. */
    ClaimOutcome outcome = ClaimOutcome::FatalStoreError;

    /** Optional implementation detail for diagnostics. */
    std::string detail;

    /**
     * Returns true when the caller should continue processing the event.
     */
    bool should_process() const;
};

/**
 * Abstract base class for webhook event-id persistence.
 *
 * Implementations provide atomic claim and mark-processed operations so
 * duplicate Stripe deliveries can be handled safely.
 */
class WebhookDedupStore {
public:
    /**
     * Destroys the store instance.
     */
    virtual ~WebhookDedupStore() = default;

    /**
     * Attempts to claim an event id for processing.
     */
    virtual ClaimResult claim(const ClaimRequest& request) = 0;

    /**
     * Marks an event id as processed.
     *
     * Returns true when an existing claim record was updated.
     */
    virtual bool mark_processed(std::string_view event_id,
                                std::chrono::system_clock::time_point processed_at) = 0;

    /**
     * Removes expired records according to retention policy.
     */
    virtual void erase_expired(std::chrono::system_clock::time_point now,
                               std::chrono::hours retention) = 0;
};

/**
 * In-memory de-duplication store for tests and local development.
 */
class InMemoryWebhookDedupStore final : public WebhookDedupStore {
public:
    /**
     * Creates an empty in-memory store.
     */
    InMemoryWebhookDedupStore();

    /**
     * Destroys the store instance.
     */
    ~InMemoryWebhookDedupStore() override;

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
    struct Entry {
        bool processed = false;
        std::chrono::system_clock::time_point claimed_at;
        std::chrono::system_clock::time_point processed_at;
    };

    std::mutex mutex;
    std::unordered_map<std::string, Entry> records;
};

/**
 * Maps a claim outcome to the default webhook HTTP response action.
 */
WebhookResponseAction map_claim_outcome_to_response(ClaimOutcome outcome);

} // namespace stripekit
