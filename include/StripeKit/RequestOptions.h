/** RequestOptions.h
 *
 * Shared per-request options for outbound Stripe API calls.
 *
 * @author Hans de Ruiter
 *
 * @license See LICENSE.md for details.
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>

namespace stripekit {

/** Minimum supported Stripe API version, also used by default for requests. */
inline constexpr const char* default_stripe_api_version = "2026-10-28.endive";

/**
 * @ingroup client
 *
 * Per-call settings accepted by every client operation.
 *
 * @code
 * stripekit::RequestOptions options;
 * options.idempotency_key = "checkout-for-order-1234";
 * options.timeout = std::chrono::seconds(30);
 * @endcode
 */
struct RequestOptions {
    /**
     * Makes the call safe to retry.
     *
     * If a response is lost and the same key is sent again, Stripe returns the
     * original result instead of performing the work twice. Set this on
     * anything that creates or charges, and derive the key from something
     * stable in the application, such as an order id.
     *
     * @sa https://docs.stripe.com/api/idempotent_requests
     */
    std::optional<std::string> idempotency_key;

    /**
     * Overrides the adapter's default timeout for this call.
     */
    std::optional<std::chrono::milliseconds> timeout;

    /**
     * Extra HTTP headers, merged over StripeKit's defaults.
     *
     * Set "Stripe-Version" to override default_stripe_api_version for this call.
     * Overrides must not predate the supported minimum and must be compatible
     * with the operation and response parser.
     */
    std::unordered_map<std::string, std::string> headers;
};

} // namespace stripekit
