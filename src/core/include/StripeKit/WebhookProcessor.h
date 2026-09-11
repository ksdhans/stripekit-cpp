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
#include "StripeKit/Webhooks.h"

#include <chrono>
#include <string>
#include <string_view>

namespace stripekit {

/**
 * @addtogroup webhooks
 * @{
 */

/**
 * Tuning for WebhookProcessor.
 */
struct WebhookProcessorOptions {
    /**
     * How long before a claim left behind by a crashed process is reclaimed.
     *
     * Set this comfortably longer than the dispatcher's worst-case runtime.
     */
    std::chrono::minutes stale_inflight_timeout = defaultStaleInflightTimeout;
};

/**
 * Handles incoming webhook deliveries through an EventDispatcher. Webhooks 
 * are HTTP requests sent by Stripe to notify about events.
 *
 * Does the same work as stripekit::webhooks::process_event_once() — verify,
 * de-duplicate, dispatch — but drives a class rather than a coroutine
 * callback, and does so synchronously. Use it when the application's webhook
 * handling is already organised as an object; otherwise
 * stripekit::webhooks::process_event_once() is the simpler route.
 *
 * The store, dispatcher, and logger are referenced, not owned, so all three
 * must outlive the processor. A single processor can serve many requests, and
 * is safe to use from several threads provided the dispatcher is.
 */
class WebhookProcessor final {
public:
    /**
     * Creates a processor.
     *
     * @param dedup_store Where claims are recorded. Must outlive the processor.
     * @param dispatcher Called for each freshly claimed event. Must outlive the
     *        processor.
     * @param logger Where to report store and dispatch failures. No logging
     *        when null.
     * @param options Claim timing.
     */
    WebhookProcessor(WebhookDedupStore& dedup_store,
                     EventDispatcher& dispatcher,
                     Logger* logger = nullptr,
                     WebhookProcessorOptions options = {});

    /**
     * Verifies, de-duplicates, and dispatches one delivery.
     *
     * @param input The raw payload, signature header, secret, and timing rules.
     * @param delivery_id Optional identifier from the adapter, passed through
     *        to the dispatcher for tracing.
     * @return Whether to answer Stripe with success, or ask it to retry.
     * @throws SignatureVerificationException The delivery cannot be trusted.
     *         Answer with HTTP 400 rather than retrying.
     * @throws ConfigurationException The payload is not a usable Stripe event.
     */
    WebhookResponseAction process(const WebhookVerificationInput& input,
                                  std::string delivery_id = {}) const;

private:
    WebhookDedupStore& dedup_store;
    EventDispatcher& dispatcher;
    Logger* logger;
    WebhookProcessorOptions options;
};

/** @} */

} // namespace stripekit