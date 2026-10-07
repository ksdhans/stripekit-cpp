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
 * @addtogroup webhooks
 * @{
 */

/**
 * Everything needed to check a webhook signature.
 */
struct WebhookVerificationInput {
    /**
     * The request body exactly as received.
     *
     * The signature covers these precise bytes, so any reformatting, parsing,
     * or character-set conversion beforehand will cause the check to fail.
     */
    std::string_view payload;

    /** Value of the "Stripe-Signature" header. */
    std::string_view stripe_signature_header;

    /** Signing secret for this endpoint, of the form "whsec_...". */
    std::string_view endpoint_secret;

    /** Current time, used to check the signature's timestamp. */
    std::chrono::system_clock::time_point now;

    /** How far the signature's timestamp may be from #now. */
    std::chrono::seconds tolerance = std::chrono::minutes(5);
};

/**
 * What was learned from a signature that checked out.
 */
struct VerificationMetadata {
    /** When Stripe signed the delivery, as a Unix timestamp. */
    std::int64_t timestamp_unix = 0;

    /** The exact string the signature was computed over. */
    std::string signed_payload;
};

/**
 * Checks that a webhook delivery really came from Stripe.
 *
 * Applications do not normally use this directly — stripekit::webhooks::construct_event()
 * and stripekit::webhooks::process_event_once() do the verification for them.
 * It is worth implementing only to substitute a different crypto backend, or to
 * inject failures in tests.
 */
class WebhookSignatureVerifier {
public:
    /**
     * Destroys the verifier instance.
     */
    virtual ~WebhookSignatureVerifier() = default;

    /**
     * Checks a signature and reports what it contained.
     *
     * @param input The payload, signature header, secret, and timing rules.
     * @return The timestamp and signed string from the verified signature.
     * @throws SignatureVerificationException The header is malformed, no
     *         signature matches, or the timestamp is outside the tolerance.
     */
    virtual VerificationMetadata verify(const WebhookVerificationInput& input) const = 0;
};

/**
 * The signature verifier StripeKit uses by default.
 *
 * Implements Stripe's scheme: HMAC-SHA256 over the timestamp and payload,
 * compared in constant time so the comparison itself leaks nothing.
 *
 * @sa https://docs.stripe.com/webhooks#verify-events
 */
class DefaultWebhookSignatureVerifier final : public WebhookSignatureVerifier {
public:
    /**
     * Checks a signature and reports what it contained.
     *
     * @param input The payload, signature header, secret, and timing rules.
     * @return The timestamp and signed string from the verified signature.
     * @throws SignatureVerificationException The header is malformed, no
     *         signature matches, or the timestamp is outside the tolerance.
     * @throws ConfigurationException This build has no cryptography support.
     */
    VerificationMetadata verify(const WebhookVerificationInput& input) const override;

    /**
     * Reports whether this build can verify signatures.
     *
     * Verification needs libsodium, which is a build option. Without it,
     * webhooks cannot be trusted and must not be accepted.
     *
     * @return True when signature verification is available.
     */
    static bool is_available();
};

/** @} */

} // namespace stripekit
