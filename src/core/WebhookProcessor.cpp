/** WebhookProcessor.cpp
 *
 * See header file for details.
 */

#include "StripeKit/WebhookProcessor.h"

#include "StripeKit/Errors.h"

#include <json/json.h>

#include <utility>

namespace stripekit {

namespace {

Json::Value parse_json(std::string_view body) {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;
    if (!reader->parse(body.data(), body.data() + body.size(), &root, &errors)) {
        throw ConfigurationException("Stripe returned invalid JSON: " + errors);
    }
    return root;
}

std::string string_value(const Json::Value& object, const char* name) {
    return object.isMember(name) && object[name].isString() ? object[name].asString() : std::string{};
}

std::int64_t integer_value(const Json::Value& object, const char* name) {
    return object.isMember(name) && object[name].isInt64() ? object[name].asInt64() : 0;
}

bool boolean_value(const Json::Value& object, const char* name) {
    return object.isMember(name) && object[name].isBool() && object[name].asBool();
}

EventEnvelope parse_event(std::string_view payload) {
    const auto object = parse_json(payload);
    const auto& request = object["request"];
    return EventEnvelope{
        .id = string_value(object, "id"),
        .type = string_value(object, "type"),
        .api_version = string_value(object, "api_version"),
        .created_unix = integer_value(object, "created"),
        .livemode = boolean_value(object, "livemode"),
        .request_id = string_value(request, "id"),
        .account = string_value(object, "account"),
        .payload_json = std::string(payload)
    };
}

} // namespace

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
    verifier.verify(input);

    EventEnvelope envelope;
    try {
        envelope = parse_event(input.payload);
    } catch (const StripeKitException&) {
        throw;
    } catch (const std::exception& error) {
        throw ConfigurationException(std::string("Unable to parse verified webhook event: ") + error.what());
    }

    if (envelope.id.empty() || envelope.type.empty()) {
        throw ConfigurationException("Verified webhook event must include id and type");
    }

    ClaimResult claim_result;
    try {
        claim_result = dedup_store.claim(ClaimRequest{
            .event_id = envelope.id,
            .now = input.now,
            .stale_inflight_timeout = options.stale_inflight_timeout
        });
    } catch (const std::exception& error) {
        log(LogLevel::Error, std::string("Webhook store claim failed: ") + error.what(), envelope);
        return WebhookResponseAction::RetryLater;
    }

    if (!claim_result.should_process()) {
        if (claim_result.outcome == ClaimOutcome::FatalStoreError) {
            log(LogLevel::Error, "Webhook store returned a fatal claim error", envelope);
        } else if (claim_result.outcome == ClaimOutcome::RetryableStoreError) {
            log(LogLevel::Warn, "Webhook store returned a retryable claim error", envelope);
        }
        return map_claim_outcome_to_response(claim_result.outcome);
    }

    EventDispatchResult dispatch_result;
    try {
        dispatch_result = dispatcher.dispatch(VerifiedEventSignal{
            .envelope = envelope,
            .delivery_id = std::move(delivery_id),
            .received_at = input.now
        });
    } catch (const std::exception& error) {
        log(LogLevel::Error, std::string("Webhook dispatch failed: ") + error.what(), envelope);
        return WebhookResponseAction::RetryLater;
    }

    if (dispatch_result == EventDispatchResult::RetryableFailure ||
        dispatch_result == EventDispatchResult::FatalFailure) {
        log(dispatch_result == EventDispatchResult::FatalFailure ? LogLevel::Error : LogLevel::Warn,
            "Webhook dispatcher requested a retry",
            envelope);
        return WebhookResponseAction::RetryLater;
    }

    try {
        if (!dedup_store.mark_processed(envelope.id, input.now)) {
            log(LogLevel::Error, "Webhook store could not mark the event as processed", envelope);
            return WebhookResponseAction::RetryLater;
        }
    } catch (const std::exception& error) {
        log(LogLevel::Error, std::string("Webhook store completion failed: ") + error.what(), envelope);
        return WebhookResponseAction::RetryLater;
    }

    return WebhookResponseAction::AcknowledgeSuccess;
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