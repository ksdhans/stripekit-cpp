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
 * @addtogroup errors
 * @{
 */

/**
 * What kind of thing went wrong, as categorised by Stripe.
 *
 * @sa https://docs.stripe.com/error-handling
 */
enum class StripeErrorType {
    Api,            /**< Something went wrong on Stripe's side. Worth retrying. */
    Card,           /**< The card was declined. Ask the customer for another one. */
    Idempotency,    /**< An idempotency key was reused with different parameters. */
    InvalidRequest, /**< The request was malformed, or referred to something that does not exist. */
    Authentication, /**< The API key is missing, wrong, or revoked. */
    Permission,     /**< The key is valid but not allowed to do this. */
    RateLimit,      /**< Too many requests too quickly. Back off and retry. */
    ApiConnection,  /**< Stripe could not be reached. Worth retrying. */
    Unknown         /**< The error could not be categorised. */
};

/**
 * The details Stripe returned about a failure.
 *
 * Always log #request_id: it identifies the exact call in Stripe's own
 * dashboard logs, which turns most "why did this fail" questions into a quick
 * lookup.
 */
struct StripeErrorPayload {
    /** What kind of thing went wrong. */
    StripeErrorType type = StripeErrorType::Unknown;

    /** Stripe's specific error code, for example "card_declined". */
    std::string code;

    /** Stripe's human-readable description of the failure. */
    std::string message;

    /** HTTP status Stripe returned, or 0 if Stripe was never reached. */
    int http_status = 0;

    /** Id of the failed request in Stripe's logs, of the form "req_...". */
    std::string request_id;

    /** True when the same request may succeed if tried again later. */
    bool retryable = false;
};

/**
 * Base class for every exception StripeKit throws.
 *
 * Catch this to handle any StripeKit failure in one place; catch the derived
 * types when the distinction matters.
 */
class StripeKitException : public std::runtime_error {
public:
    /**
     * Creates an exception carrying a descriptive message.
     *
     * @param message Text describing what went wrong.
     */
    explicit StripeKitException(const std::string& message);

    /**
     * Destroys the exception instance.
     */
    ~StripeKitException() override;
};

/**
 * Thrown when Stripe rejects a request, or cannot be reached.
 *
 * @code
 * try {
 *     const auto session = co_await stripe->checkout().sessions().create_async(request);
 * } catch (const stripekit::StripeApiException& error) {
 *     log_error(error.payload().message, error.payload().request_id);
 * }
 * @endcode
 */
class StripeApiException : public StripeKitException {
public:
    /**
     * Creates an API exception from Stripe's error details.
     *
     * @param payload The parsed error details.
     */
    explicit StripeApiException(StripeErrorPayload payload);

    /**
     * Returns the details Stripe gave for the failure.
     *
     * @return A reference valid for the lifetime of the exception.
     */
    const StripeErrorPayload& payload() const noexcept;

    /**
     * Returns whether trying the same request again may succeed.
     *
     * @return True for transient failures such as rate limits and connection
     *         problems; false for anything that will fail the same way again.
     */
    bool retryable() const noexcept;

private:
    StripeErrorPayload payload_data;
};

/**
 * Thrown when a webhook delivery cannot be trusted.
 *
 * Either the signature does not match the payload, or its timestamp is outside
 * the allowed tolerance. Answer such a request with HTTP 400: the delivery did
 * not come from Stripe, or was tampered with, and retrying will not help.
 */
class SignatureVerificationException : public StripeKitException {
public:
    /**
     * Creates a signature verification exception.
     *
     * @param message Text describing which check failed.
     */
    explicit SignatureVerificationException(const std::string& message);

    /**
     * Destroys the exception instance.
     */
    ~SignatureVerificationException() override;
};

/**
 * Thrown when StripeKit is set up in a way it cannot work with.
 *
 * Typical causes are a malformed webhook payload, or asking for a feature the
 * library was not built with. These indicate a problem to fix rather than a
 * transient failure to retry.
 */
class ConfigurationException : public StripeKitException {
public:
    /**
     * Creates a configuration exception.
     *
     * @param message Text describing what is misconfigured.
     */
    explicit ConfigurationException(const std::string& message);

    /**
     * Destroys the exception instance.
     */
    ~ConfigurationException() override;
};

/** @} */

} // namespace stripekit
