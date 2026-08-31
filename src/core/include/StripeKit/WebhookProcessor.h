/** WebhookProcessor.h
 *
 * Coordinates verified Stripe webhook event handling and de-duplication.
 *
 * @author Hans de Ruiter
 *
 * @License See LICENSE for details.
 */

#pragma once

#include "StripeKit/Events.h"
#include "StripeKit/Logging.h"
#include "StripeKit/WebhookDedupStore.h"
#include "StripeKit/WebhookVerifier.h"

#include <chrono>
#include <string>
#include <string_view>

namespace stripekit {

/**
 * Settings that control webhook de-duplication behavior.
 */
struct WebhookProcessorOptions {
    /** Timeout after which an unfinished event claim may be reclaimed. */
    std::chrono::minutes stale_inflight_timeout = defaultStaleInflightTimeout;
};

/**
 * Verifies, de-duplicates, and dispatches Stripe webhook deliveries.
 *
 * Call process for each inbound request. Signature failures are reported as
 * SignatureVerificationException so the HTTP adapter can return a client error.
 */
class WebhookProcessor final {
public:
    /**
     * Creates a processor using application-provided storage and dispatch behavior.
     */
    WebhookProcessor(WebhookDedupStore& dedup_store,
                     EventDispatcher& dispatcher,
                     Logger* logger = nullptr,
                     WebhookProcessorOptions options = {});

    /**
     * Processes one raw Stripe webhook delivery.
     *
     * Returns whether the HTTP adapter should acknowledge the delivery or ask
     * Stripe to retry it later.
     */
    WebhookResponseAction process(const WebhookVerificationInput& input,
                                  std::string delivery_id = {}) const;

private:
    void log(LogLevel level,
             std::string_view message,
             const EventEnvelope& envelope = {}) const;

    DefaultWebhookSignatureVerifier verifier;
    WebhookDedupStore& dedup_store;
    EventDispatcher& dispatcher;
    Logger* logger;
    WebhookProcessorOptions options;
};

} // namespace stripekit