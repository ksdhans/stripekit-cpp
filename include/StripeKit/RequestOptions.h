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

/**
 * Additional per-request options shared by all outbound client methods.
 */
struct RequestOptions {
    /**
     * Optional idempotency key for Stripe POST operations.
     *
     * Consumers should set this for safe retries.
     */
    std::optional<std::string> idempotency_key;

    /**
     * Optional per-request timeout override.
     *
     * When unset, adapter defaults are used.
     */
    std::optional<std::chrono::milliseconds> timeout;

    /**
     * Additional HTTP headers merged onto adapter defaults.
     */
    std::unordered_map<std::string, std::string> headers;
};

} // namespace stripekit
