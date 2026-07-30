/** WebhookVerifier.h
 * 
 * Stripe webhook signature verification interfaces and default verifier.
 * 
 * @author Hans de Ruiter
 * 
 * @license See LICENSE.md for details.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace stripekit {

/**
 * Input payload for webhook signature verification.
 */
struct WebhookVerificationInput {
    /** Raw request body bytes as received from Stripe. */
    std::string_view payload;

    /** Stripe-Signature header value. */
    std::string_view stripe_signature_header;

    /** Endpoint secret configured for the webhook destination. */
    std::string_view endpoint_secret;

    /** Current wall-clock time for timestamp tolerance checks. */
    std::chrono::system_clock::time_point now;

    /** Maximum allowed timestamp skew from Stripe signature header. */
    std::chrono::seconds tolerance = std::chrono::minutes(5);
};

/**
 * Metadata extracted during successful signature verification.
 */
struct VerificationMetadata {
    /** Timestamp parsed from the Stripe-Signature header. */
    std::int64_t timestamp_unix = 0;

    /** Concatenated string used as the signed message input. */
    std::string signed_payload;
};

/**
 * Abstract base class for validating Stripe webhook signatures.
 */
class WebhookSignatureVerifier {
public:
    /**
     * Destroys the verifier instance.
     */
    virtual ~WebhookSignatureVerifier() = default;

    /**
     * Verifies the Stripe webhook signature and returns parsed metadata.
     *
     * Throws SignatureVerificationException on validation failures.
     */
    virtual VerificationMetadata verify(const WebhookVerificationInput& input) const = 0;
};

/**
 * Default webhook signature verifier implementation.
 */
class DefaultWebhookSignatureVerifier final : public WebhookSignatureVerifier {
public:
    /**
     * Verifies a Stripe webhook signature.
     */
    VerificationMetadata verify(const WebhookVerificationInput& input) const override;

    /**
     * Returns true when signature verification support is available.
     */
    static bool is_available();
};

} // namespace stripekit
