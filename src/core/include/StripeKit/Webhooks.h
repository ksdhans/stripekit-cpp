/** Webhooks.h
 *
 * Stripe-style webhook construction and de-duplicated processing helpers.
 *
 * @author Hans de Ruiter
 *
 * @License See LICENSE for details.
 */

#pragma once

#include "StripeKit/Async.h"
#include "StripeKit/Events.h"
#include "StripeKit/Logging.h"
#include "StripeKit/WebhookDedupStore.h"
#include "StripeKit/WebhookVerifier.h"

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

namespace stripekit::webhooks {

/** Parsed Stripe webhook event. */
using Event = EventEnvelope;

/** Options used when verifying and constructing a Stripe webhook event. */
struct ConstructEventOptions {
    /** Current wall-clock time for signature tolerance checks. */
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    /** Maximum allowed timestamp skew from Stripe's signature header. */
    std::chrono::seconds tolerance = std::chrono::minutes(5);
};

/** Result returned by application webhook handlers. */
enum class ProcessResult {
    Handled,
    Ignored,
    RetryableFailure,
    FatalFailure
};

/** Application coroutine callback used by process_event_once. */
using EventHandler = std::function<Task<ProcessResult>(const Event&)>;

/** Options used when processing a Stripe webhook event once. */
struct ProcessEventOptions {
    /** Current wall-clock time for signature verification and de-duplication. */
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    /** Maximum allowed timestamp skew from Stripe's signature header. */
    std::chrono::seconds tolerance = std::chrono::minutes(5);

    /** Timeout after which an unfinished event claim may be reclaimed. */
    std::chrono::minutes stale_inflight_timeout = defaultStaleInflightTimeout;

    /** Optional log sink for store and handler failures. */
    Logger* logger = nullptr;
};

/** Summary returned by process_event_once. */
struct ProcessEventOnceResult {
    /** HTTP response action an adapter should apply. */
    WebhookResponseAction response_action = WebhookResponseAction::RetryLater;

    /** De-duplication claim outcome for this event. */
    ClaimOutcome claim_outcome = ClaimOutcome::FatalStoreError;

    /** Stripe event id, when available. */
    std::string event_id;

    /** Stripe event type, when available. */
    std::string event_type;
};

/**
 * Verifies a Stripe webhook signature and parses the event envelope.
 *
 * Throws SignatureVerificationException for invalid signatures and
 * ConfigurationException for malformed event payloads.
 */
Event construct_event(std::string_view payload,
                      std::string_view signature_header,
                      std::string_view endpoint_secret,
                      ConstructEventOptions options = {});

/**
 * De-duplicates and dispatches a previously constructed Stripe webhook event once.
 */
Task<ProcessEventOnceResult> process_event_once(const Event& event,
                                                WebhookDedupStore& dedup_store,
                                                EventHandler handler,
                                                ProcessEventOptions options = {});

/**
 * Verifies, de-duplicates, and dispatches a Stripe webhook event once.
 */
Task<ProcessEventOnceResult> process_event_once(std::string_view payload,
                                                std::string_view signature_header,
                                                std::string_view endpoint_secret,
                                                WebhookDedupStore& dedup_store,
                                                EventHandler handler,
                                                ProcessEventOptions options = {});

} // namespace stripekit::webhooks