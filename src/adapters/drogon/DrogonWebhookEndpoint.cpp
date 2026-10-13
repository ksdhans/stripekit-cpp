/** DrogonWebhookEndpoint.cpp
 *
 * See header file for details.
 */

#include "StripeKit/Adapters/Drogon.h"

#include "StripeKit/Errors.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <chrono>
#include <functional>
#include <utility>

namespace stripekit::drogon {

namespace {

void log_webhook_failure(Logger* logger, std::string_view message) {
    if (logger != nullptr && logger->should_log(LogLevel::Error)) {
        logger->log(LogLevel::Error,
                    redact_sensitive_text(message),
                    LogContext{ .adapter_name = "drogon" });
    }
}

/**
 * Verifies and handles a webhook request as a Drogon coroutine.
 *
 * @param options Signing, deduplication, and handler settings for the route.
 * @param payload The request body.
 * @param signature Stripe's signature header.
 * @return The HTTP response for this delivery.
 */
::drogon::Task<::drogon::HttpResponsePtr>
handle_webhook_request(WebhookHandlerOptions options, std::string payload, std::string signature) {
    auto response = ::drogon::HttpResponse::newHttpResponse();
    try {
        const auto result = co_await webhooks::process_event_once(
            payload,
            signature,
            options.endpoint_secret,
            options.dedup_store,
            options.handler,
            webhooks::ProcessEventOptions{ .now = std::chrono::system_clock::now(),
                                           .tolerance = options.tolerance,
                                           .stale_inflight_timeout = options.stale_inflight_timeout,
                                           .logger = options.logger });

        response->setStatusCode(result.response_action == WebhookResponseAction::AcknowledgeSuccess
                                    ? ::drogon::k200OK
                                    : ::drogon::k500InternalServerError);
    } catch (const SignatureVerificationException& error) {
        log_webhook_failure(options.logger,
                            std::string("Webhook signature verification failed: ") + error.what());
        response->setStatusCode(::drogon::k400BadRequest);
    } catch (const StripeKitException& error) {
        log_webhook_failure(options.logger,
                            std::string("Webhook processing failed: ") + error.what());
        response->setStatusCode(::drogon::k400BadRequest);
    } catch (const std::exception& error) {
        log_webhook_failure(options.logger,
                            std::string("Webhook processing threw: ") + error.what());
        response->setStatusCode(::drogon::k500InternalServerError);
    } catch (...) {
        log_webhook_failure(options.logger, "Webhook processing threw an unknown exception");
        response->setStatusCode(::drogon::k500InternalServerError);
    }
    co_return response;
}

} // namespace

WebhookEndpoint::WebhookEndpoint(WebhookProcessor& processor, std::string endpoint_secret)
    : processor(processor), endpoint_secret(std::move(endpoint_secret)) {}

void WebhookEndpoint::register_handler(std::string path) const {
    WebhookProcessor* processor_ptr = &processor;
    std::string endpoint_secret_copy = endpoint_secret;
    ::drogon::app().registerHandler(
        path,
        [processor_ptr, endpoint_secret = std::move(endpoint_secret_copy)](
            const ::drogon::HttpRequestPtr& request,
            std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
            auto response = ::drogon::HttpResponse::newHttpResponse();
            try {
                const auto action = processor_ptr->process(
                    WebhookVerificationInput{ .payload = request->body(),
                                              .stripe_signature_header =
                                                  request->getHeader("stripe-signature"),
                                              .endpoint_secret = endpoint_secret,
                                              .now = std::chrono::system_clock::now() },
                    request->getHeader("stripe-webhook-id"));
                response->setStatusCode(action == WebhookResponseAction::AcknowledgeSuccess
                                            ? ::drogon::k200OK
                                            : ::drogon::k500InternalServerError);
            } catch (const SignatureVerificationException&) {
                response->setStatusCode(::drogon::k400BadRequest);
            } catch (const StripeKitException&) {
                response->setStatusCode(::drogon::k400BadRequest);
            } catch (...) {
                response->setStatusCode(::drogon::k500InternalServerError);
            }
            callback(response);
        },
        { ::drogon::Post });
}

void register_webhook_handler(WebhookHandlerOptions options) {
    const std::string path = options.path;
    ::drogon::app().registerHandler(
        path,
        [options = std::move(options)](
            ::drogon::HttpRequestPtr request) -> ::drogon::Task<::drogon::HttpResponsePtr> {
            return handle_webhook_request(
                options, std::string(request->body()), request->getHeader("stripe-signature"));
        },
        { ::drogon::Post });
}

} // namespace stripekit::drogon