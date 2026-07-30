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
 * Outcome of dispatching a verified webhook event signal.
 */
enum class EventDispatchResult {
    Handled,
    Ignored,
    RetryableFailure,
    FatalFailure
};

/**
 * Represents a verified and parsed Stripe event signal emitted by core.
 */
struct VerifiedEventSignal {
    /** Parsed Stripe event envelope. */
    EventEnvelope envelope;

    /** Delivery identifier from the inbound webhook layer, when available. */
    std::string delivery_id;

    /** Receipt timestamp assigned by the inbound adapter. */
    std::chrono::system_clock::time_point received_at;
};

/**
 * Abstract base class for handling verified Stripe webhook events.
 */
class EventDispatcher {
public:
    /**
     * Destroys the dispatcher instance.
     */
    virtual ~EventDispatcher() = default;

    /**
     * Dispatches a verified event signal.
     */
    virtual EventDispatchResult dispatch(const VerifiedEventSignal& signal) = 0;
};

} // namespace stripekit
