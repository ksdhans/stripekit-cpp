/** Events.h
 *
 * Verified webhook signal structures and dispatch interfaces.
 *
 * @author Hans de Ruiter
 *
 * @license See LICENSE.md for details.
 */

#pragma once

#include "StripeKit/Models.h"

#include <chrono>
#include <string>

namespace stripekit {

/**
 * @addtogroup webhooks
 * @{
 */

/**
 * What a dispatcher reports back about an event.
 *
 * The class-based counterpart of stripekit::webhooks::ProcessResult.
 */
enum class EventDispatchResult {
    Handled,          /**< Dealt with. Stripe is told the delivery succeeded. */
    Ignored,          /**< Not interesting to this application. Also reported as success. */
    RetryableFailure, /**< Something was temporarily unavailable. Stripe will deliver again. */
    FatalFailure      /**< Cannot be handled, ever. Stripe is told to stop retrying. */
};

/**
 * A verified event, with the delivery details that came with it.
 */
struct VerifiedEventSignal {
    /** The event itself. */
    EventEnvelope envelope;

    /** Identifier the adapter assigned to this delivery, for tracing. May be empty. */
    std::string delivery_id;

    /** When the adapter received the delivery. */
    std::chrono::system_clock::time_point received_at;
};

/**
 * Handles verified webhook events, for applications using WebhookProcessor.
 *
 * Implement this when webhook handling is better expressed as a class than as
 * a callback; otherwise use stripekit::webhooks::EventHandler.
 *
 * dispatch() is called once per event, only after the signature has been
 * checked and the event id claimed, so it never sees a duplicate. It must
 * return quickly, since Stripe treats a slow endpoint as a failed delivery.
 */
class EventDispatcher {
public:
    /**
     * Destroys the dispatcher instance.
     */
    virtual ~EventDispatcher() = default;

    /**
     * Handles one verified event.
     *
     * @param signal The event and its delivery details.
     * @return What happened, which decides what Stripe is told. Returning
     *         EventDispatchResult::RetryableFailure asks Stripe to deliver
     *         again later.
     */
    virtual EventDispatchResult dispatch(const VerifiedEventSignal& signal) = 0;
};

/** @} */

} // namespace stripekit
