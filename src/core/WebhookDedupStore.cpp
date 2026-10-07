/** WebhookDedupStore.cpp
 *
 * See header file for details.
 */

#include "StripeKit/WebhookDedupStore.h"

namespace stripekit {

bool ClaimResult::should_process() const {
    return outcome == ClaimOutcome::Claimed;
}

InMemoryWebhookDedupStore::InMemoryWebhookDedupStore() = default;

InMemoryWebhookDedupStore::~InMemoryWebhookDedupStore() = default;

ClaimResult InMemoryWebhookDedupStore::claim(const ClaimRequest& request) {
    std::lock_guard<std::mutex> lock(mutex);

    auto it = records.find(request.event_id);
    if (it == records.end()) {
        records.emplace(request.event_id,
                        Entry{ .processed = false,
                               .claimed_at = request.now,
                               .processed_at = std::chrono::system_clock::time_point{} });
        return ClaimResult{ .outcome = ClaimOutcome::Claimed, .detail = "first-seen" };
    }

    if (it->second.processed) {
        return ClaimResult{ .outcome = ClaimOutcome::DuplicateProcessed,
                            .detail = "already-processed" };
    }

    const auto age = request.now - it->second.claimed_at;
    if (age >= request.stale_inflight_timeout) {
        it->second.claimed_at = request.now;
        return ClaimResult{ .outcome = ClaimOutcome::Claimed,
                            .detail = "reclaimed-stale-inflight" };
    }

    return ClaimResult{ .outcome = ClaimOutcome::DuplicateInflight,
                        .detail = "already-claimed-inflight" };
}

bool InMemoryWebhookDedupStore::mark_processed(std::string_view event_id,
                                               std::chrono::system_clock::time_point processed_at) {
    std::lock_guard<std::mutex> lock(mutex);

    auto it = records.find(std::string(event_id));
    if (it == records.end()) {
        return false;
    }

    it->second.processed = true;
    it->second.processed_at = processed_at;
    return true;
}

void InMemoryWebhookDedupStore::erase_expired(std::chrono::system_clock::time_point now,
                                              std::chrono::hours retention) {
    std::lock_guard<std::mutex> lock(mutex);

    for (auto it = records.begin(); it != records.end();) {
        const auto reference_time =
            it->second.processed ? it->second.processed_at : it->second.claimed_at;
        if (reference_time.time_since_epoch().count() > 0 && (now - reference_time) >= retention) {
            it = records.erase(it);
            continue;
        }
        ++it;
    }
}

WebhookResponseAction map_claim_outcome_to_response(ClaimOutcome outcome) {
    switch (outcome) {
    case ClaimOutcome::Claimed:
        return WebhookResponseAction::AcknowledgeSuccess;
    case ClaimOutcome::DuplicateProcessed:
        return WebhookResponseAction::AcknowledgeSuccess;
    case ClaimOutcome::DuplicateInflight:
        return WebhookResponseAction::RetryLater;
    case ClaimOutcome::RetryableStoreError:
        return WebhookResponseAction::RetryLater;
    case ClaimOutcome::FatalStoreError:
        return WebhookResponseAction::RetryLater;
    }

    return WebhookResponseAction::RetryLater;
}

} // namespace stripekit
