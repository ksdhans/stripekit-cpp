/** Webhooks.cpp
 *
 * See header file for details.
 */

#include "StripeKit/Webhooks.h"

#include "JsonValue.h"
#include "StripeKit/Errors.h"

#include <string>

namespace stripekit::webhooks {

namespace {

Event parse_event(std::string_view payload) {
    const auto object = json_value::parse(payload);
    const auto& request = object["request"];
    const std::string request_id =
        request.isString() ? request.asString() : json_value::string(request, "id");
    Event event{ .id = json_value::string(object, "id"),
                 .type = json_value::string(object, "type"),
                 .api_version = json_value::string(object, "api_version"),
                 .created_unix = json_value::integer(object, "created"),
                 .livemode = json_value::boolean(object, "livemode"),
                 .request_id = request_id,
                 .account = json_value::string(object, "account"),
                 .payload_json = std::string(payload) };

    if (event.id.empty() || event.type.empty()) {
        throw ConfigurationException("Verified webhook event must include id and type");
    }

    return event;
}

void log_message(Logger* logger, LogLevel level, std::string_view message, const Event& event) {
    if (logger == nullptr || !logger->should_log(level)) {
        return;
    }

    logger->log(level,
                redact_sensitive_text(message),
                LogContext{ .request_id = event.request_id,
                            .event_id = event.id,
                            .webhook_type = event.type,
                            .adapter_name = "core" });
}

ProcessEventOnceResult
result_for(const Event& event, ClaimOutcome claim_outcome, WebhookResponseAction response_action) {
    return ProcessEventOnceResult{ .response_action = response_action,
                                   .claim_outcome = claim_outcome,
                                   .event_id = event.id,
                                   .event_type = event.type };
}

/** Makes a finished, failed attempt available to the next delivery. */
void release_failed_claim(WebhookDedupStore& store,
                          const Event& event,
                          const ProcessEventOptions& options) {
    try {
        if (!store.release_claim(event.id, options.now)) {
            log_message(options.logger,
                        LogLevel::Error,
                        "Webhook store could not release the failed claim",
                        event);
        }
    } catch (const std::exception& error) {
        log_message(options.logger,
                    LogLevel::Error,
                    std::string("Webhook store claim release failed: ") + error.what(),
                    event);
    }
}

} // namespace

Event construct_event(std::string_view payload,
                      std::string_view signature_header,
                      std::string_view endpoint_secret,
                      ConstructEventOptions options) {
    DefaultWebhookSignatureVerifier verifier;
    verifier.verify(WebhookVerificationInput{ .payload = payload,
                                              .stripe_signature_header = signature_header,
                                              .endpoint_secret = endpoint_secret,
                                              .now = options.now,
                                              .tolerance = options.tolerance });

    try {
        return parse_event(payload);
    } catch (const StripeKitException&) {
        throw;
    } catch (const std::exception& error) {
        throw ConfigurationException(std::string("Unable to parse verified webhook event: ") +
                                     error.what());
    }
}

Task<ProcessEventOnceResult> process_event_once(const Event& event,
                                                WebhookDedupStore& dedup_store,
                                                EventHandler handler,
                                                ProcessEventOptions options) {
    ClaimResult claim_result;
    try {
        claim_result = dedup_store.claim(
            ClaimRequest{ .event_id = event.id,
                          .now = options.now,
                          .stale_inflight_timeout = options.stale_inflight_timeout });
    } catch (const std::exception& error) {
        log_message(options.logger,
                    LogLevel::Error,
                    std::string("Webhook store claim failed: ") + error.what(),
                    event);
        co_return result_for(
            event, ClaimOutcome::RetryableStoreError, WebhookResponseAction::RetryLater);
    }

    if (!claim_result.should_process()) {
        if (claim_result.outcome == ClaimOutcome::FatalStoreError) {
            log_message(options.logger,
                        LogLevel::Error,
                        "Webhook store returned a fatal claim error",
                        event);
        } else if (claim_result.outcome == ClaimOutcome::RetryableStoreError) {
            log_message(options.logger,
                        LogLevel::Warn,
                        "Webhook store returned a retryable claim error",
                        event);
        }
        co_return result_for(
            event, claim_result.outcome, map_claim_outcome_to_response(claim_result.outcome));
    }

    ProcessResult process_result;
    try {
        process_result = co_await handler(event);
    } catch (const std::exception& error) {
        log_message(options.logger,
                    LogLevel::Error,
                    std::string("Webhook handler failed: ") + error.what(),
                    event);
        release_failed_claim(dedup_store, event, options);
        co_return result_for(event, claim_result.outcome, WebhookResponseAction::RetryLater);
    } catch (...) {
        log_message(options.logger,
                    LogLevel::Error,
                    "Webhook handler failed with an unknown exception",
                    event);
        release_failed_claim(dedup_store, event, options);
        co_return result_for(event, claim_result.outcome, WebhookResponseAction::RetryLater);
    }

    if (process_result == ProcessResult::RetryableFailure ||
        process_result == ProcessResult::FatalFailure) {
        log_message(options.logger,
                    process_result == ProcessResult::FatalFailure ? LogLevel::Error
                                                                  : LogLevel::Warn,
                    "Webhook handler requested a retry",
                    event);
        release_failed_claim(dedup_store, event, options);
        co_return result_for(event, claim_result.outcome, WebhookResponseAction::RetryLater);
    }

    try {
        if (!dedup_store.mark_processed(event.id, options.now)) {
            log_message(options.logger,
                        LogLevel::Error,
                        "Webhook store could not mark the event as processed",
                        event);
            release_failed_claim(dedup_store, event, options);
            co_return result_for(event, claim_result.outcome, WebhookResponseAction::RetryLater);
        }
    } catch (const std::exception& error) {
        log_message(options.logger,
                    LogLevel::Error,
                    std::string("Webhook store completion failed: ") + error.what(),
                    event);
        release_failed_claim(dedup_store, event, options);
        co_return result_for(event, claim_result.outcome, WebhookResponseAction::RetryLater);
    }

    co_return result_for(event, claim_result.outcome, WebhookResponseAction::AcknowledgeSuccess);
}

Task<ProcessEventOnceResult> process_event_once(std::string_view payload,
                                                std::string_view signature_header,
                                                std::string_view endpoint_secret,
                                                WebhookDedupStore& dedup_store,
                                                EventHandler handler,
                                                ProcessEventOptions options) {
    Event event = construct_event(
        payload,
        signature_header,
        endpoint_secret,
        ConstructEventOptions{ .now = options.now, .tolerance = options.tolerance });

    co_return co_await process_event_once(event, dedup_store, std::move(handler), options);
}

} // namespace stripekit::webhooks