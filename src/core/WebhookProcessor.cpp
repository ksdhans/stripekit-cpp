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
    : dedup_store(dedup_store),
      dispatcher(dispatcher),
      logger(logger),
      options(options) {
}

WebhookResponseAction WebhookProcessor::process(const WebhookVerificationInput& input,
                                                std::string delivery_id) const {
    const auto result = webhooks::process_event_once(
        input.payload,
        input.stripe_signature_header,
        input.endpoint_secret,
        dedup_store,
        [this, &delivery_id, &input](const webhooks::Event& event) {
            const EventDispatchResult dispatch_result = dispatcher.dispatch(VerifiedEventSignal{
                .envelope = event,
                .delivery_id = std::move(delivery_id),
                .received_at = input.now
            });

            switch (dispatch_result) {
            case EventDispatchResult::Handled:
                return webhooks::ProcessResult::Handled;
            case EventDispatchResult::Ignored:
                return webhooks::ProcessResult::Ignored;
            case EventDispatchResult::RetryableFailure:
                return webhooks::ProcessResult::RetryableFailure;
            case EventDispatchResult::FatalFailure:
                return webhooks::ProcessResult::FatalFailure;
            }

            return webhooks::ProcessResult::FatalFailure;
        },
        webhooks::ProcessEventOptions{
            .now = input.now,
            .tolerance = input.tolerance,
            .stale_inflight_timeout = options.stale_inflight_timeout,
            .logger = logger
        });

    return result.response_action;
}

void WebhookProcessor::log(LogLevel level,
                           std::string_view message,
                           const EventEnvelope& envelope) const {
    if (logger == nullptr || !logger->should_log(level)) {
        return;
    }

    logger->log(level,
                message,
                LogContext{
                    .request_id = envelope.request_id,
                    .event_id = envelope.id,
                    .webhook_type = envelope.type,
                    .adapter_name = "core"
                });
}

} // namespace stripekit