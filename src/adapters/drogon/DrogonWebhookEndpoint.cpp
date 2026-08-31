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

WebhookEndpoint::WebhookEndpoint(WebhookProcessor& processor, std::string endpoint_secret)
    : processor(processor), endpoint_secret(std::move(endpoint_secret)) {}

void WebhookEndpoint::register_handler(std::string path) const {
    ::drogon::app().registerHandler(path,
        [this](const ::drogon::HttpRequestPtr& request,
               std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
            auto response = ::drogon::HttpResponse::newHttpResponse();
            try {
                const auto action = processor.process(WebhookVerificationInput{
                    .payload = request->body(),
                    .stripe_signature_header = request->getHeader("stripe-signature"),
                    .endpoint_secret = endpoint_secret,
                    .now = std::chrono::system_clock::now()
                }, request->getHeader("stripe-webhook-id"));
                response->setStatusCode(action == WebhookResponseAction::AcknowledgeSuccess
                    ? ::drogon::k200OK : ::drogon::k500InternalServerError);
            } catch (const SignatureVerificationException&) {
                response->setStatusCode(::drogon::k400BadRequest);
            } catch (const StripeKitException&) {
                response->setStatusCode(::drogon::k400BadRequest);
            } catch (...) {
                response->setStatusCode(::drogon::k500InternalServerError);
            }
            callback(response);
        }, {::drogon::Post});
}

void register_webhook_handler(WebhookHandlerOptions options) {
    ::drogon::app().registerHandler(options.path,
        [options = std::move(options)](const ::drogon::HttpRequestPtr& request,
                                       std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) mutable {
            auto response = ::drogon::HttpResponse::newHttpResponse();
            try {
                const auto result = webhooks::process_event_once(
                    request->body(),
                    request->getHeader("stripe-signature"),
                    options.endpoint_secret,
                    options.dedup_store,
                    options.handler,
                    webhooks::ProcessEventOptions{
                        .now = std::chrono::system_clock::now(),
                        .tolerance = options.tolerance,
                        .stale_inflight_timeout = options.stale_inflight_timeout,
                        .logger = options.logger
                    });

                response->setStatusCode(result.response_action == WebhookResponseAction::AcknowledgeSuccess
                    ? ::drogon::k200OK : ::drogon::k500InternalServerError);
            } catch (const SignatureVerificationException&) {
                response->setStatusCode(::drogon::k400BadRequest);
            } catch (const StripeKitException&) {
                response->setStatusCode(::drogon::k400BadRequest);
            } catch (...) {
                response->setStatusCode(::drogon::k500InternalServerError);
            }
            callback(response);
        }, {::drogon::Post});
}

} // namespace stripekit::drogon