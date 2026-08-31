/** DrogonWebhookEndpoint.cpp
 *
 * See header file for details.
 */

#include "StripeKit/Drogon.h"

#include "StripeKit/Errors.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <chrono>
#include <functional>
#include <utility>

namespace stripekit {

DrogonWebhookEndpoint::DrogonWebhookEndpoint(WebhookProcessor& processor, std::string endpoint_secret)
    : processor(processor), endpoint_secret(std::move(endpoint_secret)) {}

void DrogonWebhookEndpoint::register_handler(std::string path) const {
    drogon::app().registerHandler(path,
        [this](const drogon::HttpRequestPtr& request,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            auto response = drogon::HttpResponse::newHttpResponse();
            try {
                const auto action = processor.process(WebhookVerificationInput{
                    .payload = request->body(),
                    .stripe_signature_header = request->getHeader("stripe-signature"),
                    .endpoint_secret = endpoint_secret,
                    .now = std::chrono::system_clock::now()
                }, request->getHeader("stripe-webhook-id"));
                response->setStatusCode(action == WebhookResponseAction::AcknowledgeSuccess
                    ? drogon::k200OK : drogon::k500InternalServerError);
            } catch (const SignatureVerificationException&) {
                response->setStatusCode(drogon::k400BadRequest);
            } catch (const StripeKitException&) {
                response->setStatusCode(drogon::k400BadRequest);
            } catch (...) {
                response->setStatusCode(drogon::k500InternalServerError);
            }
            callback(response);
        }, {drogon::Post});
}

} // namespace stripekit