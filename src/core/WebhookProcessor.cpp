/** WebhookProcessor.cpp
 *
 * See header file for details.
 */

#include "StripeKit/WebhookProcessor.h"

#include <utility>

namespace stripekit {

WebhookProcessor::WebhookProcessor(WebhookDedupStore& dedup_store,
                                   EventDispatcher& dispatcher,
                                   Logger* logger,
                                   WebhookProcessorOptions options)
    : dedup_store(dedup_store), dispatcher(dispatcher), logger(logger), options(options) {}

WebhookResponseAction WebhookProcessor::process(const WebhookVerificationInput& input,
                                                std::string delivery_id) const {
    auto task = webhooks::process_event_once(
        input.payload,
        input.stripe_signature_header,
        input.endpoint_secret,
        dedup_store,
        [this, &delivery_id, &input](
            const webhooks::Event& event) -> Task<webhooks::ProcessResult> {
            co_return dispatcher.dispatch(
                VerifiedEventSignal{ .envelope = event,
                                     .delivery_id = std::move(delivery_id),
                                     .received_at = input.now });
        },
        webhooks::ProcessEventOptions{ .now = input.now,
                                       .tolerance = input.tolerance,
                                       .stale_inflight_timeout = options.stale_inflight_timeout,
                                       .logger = logger });

    const auto result = task.result();
    return result.response_action;
}

} // namespace stripekit