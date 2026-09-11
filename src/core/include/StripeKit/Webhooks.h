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

/**
 * @addtogroup webhooks
 * @{
 */

/** A verified Stripe webhook event. */
using Event = EventEnvelope;

/** Tuning for construct_event(). The defaults are what Stripe recommends. */
struct ConstructEventOptions {
    /** Current time, used to check the signature's timestamp. Override in tests. */
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    /**
     * How far the signature's timestamp may be from #now.
     *
     * This is what stops an old delivery being replayed. Widening it past a
     * few minutes weakens that protection; if verification fails because the
     * clocks disagree, fix the clock rather than the tolerance.
     */
    std::chrono::seconds tolerance = std::chrono::minutes(5);
};

/** What a handler reports back about an event. */
enum class ProcessResult {
    Handled,          /**< Dealt with. Stripe is told the delivery succeeded. */
    Ignored,          /**< Not interesting to this application. Also reported as success. */
    RetryableFailure, /**< Something was temporarily unavailable. Stripe will deliver again. */
    FatalFailure      /**< Cannot be handled, ever. Stripe is told to stop retrying. */
};

/**
 * The application's webhook handler.
 *
 * Called once per event, only after the signature has been checked and the
 * event id claimed. Return quickly: Stripe treats a slow endpoint as a failed
 * delivery, so queue any long work rather than doing it here.
 */
using EventHandler = std::function<Task<ProcessResult>(const Event&)>;

/** Tuning for process_event_once(). The defaults suit most applications. */
struct ProcessEventOptions {
    /** Current time, used for signature checks and claim timing. Override in tests. */
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    /** How far the signature's timestamp may be from #now. */
    std::chrono::seconds tolerance = std::chrono::minutes(5);

    /**
     * How long before a claim left behind by a crashed process is reclaimed.
     *
     * Set this comfortably longer than the handler's worst-case runtime, or a
     * slow handler risks a second delivery being let through while it is still
     * working.
     */
    std::chrono::minutes stale_inflight_timeout = defaultStaleInflightTimeout;

    /** Where to report store and handler failures. No logging when null. */
    Logger* logger = nullptr;
};

/** What happened to a webhook delivery. */
struct ProcessEventOnceResult {
    /** Whether to answer Stripe with success, or ask it to retry. */
    WebhookResponseAction response_action = WebhookResponseAction::RetryLater;

    /** Whether this delivery was the one that got to run the handler. */
    ClaimOutcome claim_outcome = ClaimOutcome::FatalStoreError;

    /** Id of the event, when it could be parsed. */
    std::string event_id;

    /** Type of the event, when it could be parsed. */
    std::string event_type;
};

/**
 * Checks that a webhook delivery really came from Stripe, and parses it.
 *
 * Pass the request body exactly as received. The signature covers those precise
 * bytes, so anything that re-encodes or reformats the body first will cause
 * verification to fail.
 *
 * This verifies but does not de-duplicate. Stripe delivers the same event more
 * than once, so a handler that changes state should use process_event_once()
 * instead.
 *
 * @code
 * const auto event = stripekit::webhooks::construct_event(
 *     request_body, request.header("stripe-signature"), endpoint_secret);
 * @endcode
 *
 * @param payload Raw request body, unmodified.
 * @param signature_header Value of the "Stripe-Signature" header.
 * @param endpoint_secret Signing secret for this endpoint, of the form
 *        "whsec_...". It differs between the Stripe CLI and each registered
 *        endpoint.
 * @param options Timestamp tolerance and current time.
 * @return The verified event.
 * @throws SignatureVerificationException The signature does not match, or its
 *         timestamp is outside the tolerance. Answer the request with HTTP 400.
 * @throws ConfigurationException The payload is not a usable Stripe event.
 *
 * @sa https://docs.stripe.com/webhooks#verify-events
 */
Event construct_event(std::string_view payload,
                      std::string_view signature_header,
                      std::string_view endpoint_secret,
                      ConstructEventOptions options = {});

/**
 * Runs the handler for an already-verified event, at most once.
 *
 * Reserves the event id in @p dedup_store first, so repeated deliveries of the
 * same event reach the handler only the first time. Use this overload when the
 * event has already been through construct_event().
 *
 * @param event The verified event.
 * @param dedup_store Where claims are recorded. Must outlive the call, and must
 *        be shared by every process that handles this endpoint.
 * @param handler Called only for the delivery that wins the claim.
 * @param options Claim timing and logging.
 * @return What happened, including the status to send Stripe.
 */
Task<ProcessEventOnceResult> process_event_once(const Event& event,
                                                WebhookDedupStore& dedup_store,
                                                EventHandler handler,
                                                ProcessEventOptions options = {});

/**
 * Verifies a webhook delivery and runs the handler for it, at most once.
 *
 * This is the whole inbound path in one call: check the signature, reserve the
 * event id, run the handler, and report the status to send back to Stripe. It
 * is what an adapter's webhook route should call.
 *
 * @code
 * auto result = co_await stripekit::webhooks::process_event_once(
 *     body, signature_header, endpoint_secret, dedup_store,
 *     [](const stripekit::webhooks::Event& event)
 *         -> stripekit::Task<stripekit::webhooks::ProcessResult> {
 *         co_return stripekit::webhooks::ProcessResult::Handled;
 *     });
 * @endcode
 *
 * @param payload Raw request body, unmodified.
 * @param signature_header Value of the "Stripe-Signature" header.
 * @param endpoint_secret Signing secret for this endpoint.
 * @param dedup_store Where claims are recorded. Must outlive the call.
 * @param handler Called only for the delivery that wins the claim.
 * @param options Tolerance, claim timing, and logging.
 * @return What happened, including the status to send Stripe.
 * @throws SignatureVerificationException The delivery cannot be trusted.
 *         Answer with HTTP 400 rather than retrying.
 * @throws ConfigurationException The payload is not a usable Stripe event.
 */
Task<ProcessEventOnceResult> process_event_once(std::string_view payload,
                                                std::string_view signature_header,
                                                std::string_view endpoint_secret,
                                                WebhookDedupStore& dedup_store,
                                                EventHandler handler,
                                                ProcessEventOptions options = {});

/** @} */

} // namespace stripekit::webhooks