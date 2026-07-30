/** Errors.h
 * 
 * Exception and error payload types for StripeKit public APIs.
 * 
 * @author Hans de Ruiter
 * 
 * @license See LICENSE.md for details.
 */

#pragma once

#include <stdexcept>
#include <string>

namespace stripekit {

/**
 * Stripe API error categories.
 */
enum class StripeErrorType {
    Api,
    Card,
    Idempotency,
    InvalidRequest,
    Authentication,
    Permission,
    RateLimit,
    ApiConnection,
    Unknown
};

/**
 * Carries Stripe error information for API failures.
 */
struct StripeErrorPayload {
    /** High-level Stripe error category. */
    StripeErrorType type = StripeErrorType::Unknown;

    /** Stripe error code field when available. */
    std::string code;

    /** Human-readable error message from Stripe. */
    std::string message;

    /** HTTP status returned by Stripe. */
    int http_status = 0;

    /** Stripe request identifier associated with the failure. */
    std::string request_id;

    /** True when retrying the same request may succeed. */
    bool retryable = false;
};

/**
 * Base class for StripeKit public exceptions.
 */
class StripeKitException : public std::runtime_error {
public:
    /**
     * Creates a StripeKitException with a descriptive message.
     */
    explicit StripeKitException(const std::string& message);

    /**
     * Destroys the exception instance.
     */
    ~StripeKitException() override;
};

/**
 * Exception raised for Stripe API failures.
 */
class StripeApiException : public StripeKitException {
public:
    /**
     * Creates an API exception with a normalized Stripe payload.
     */
    explicit StripeApiException(StripeErrorPayload payload);

    /**
     * Returns the normalized Stripe error payload.
     */
    const StripeErrorPayload& payload() const noexcept;

    /**
     * Returns Stripe retryability information from the payload.
     */
    bool retryable() const noexcept;

private:
    StripeErrorPayload payload_data;
};

/**
 * Exception raised when webhook signature verification fails.
 */
class SignatureVerificationException : public StripeKitException {
public:
    /**
     * Creates a signature verification exception.
     */
    explicit SignatureVerificationException(const std::string& message);

    /**
     * Destroys the exception instance.
     */
    ~SignatureVerificationException() override;
};

/**
 * Exception raised for invalid or unsupported runtime configuration.
 */
class ConfigurationException : public StripeKitException {
public:
    /**
     * Creates a configuration exception.
     */
    explicit ConfigurationException(const std::string& message);

    /**
     * Destroys the exception instance.
     */
    ~ConfigurationException() override;
};

} // namespace stripekit
