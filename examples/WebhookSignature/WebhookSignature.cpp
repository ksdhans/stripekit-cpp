/** WebhookSignature.cpp
 *
 * See README.md for details.
 */

#include "StripeKit/Errors.h"
#include "StripeKit/Webhooks.h"

#include <drogon/drogon.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

std::string env_or(const char* name, std::string fallback) {
    const char* value = std::getenv(name);
    return value == nullptr || std::string(value).empty() ? std::move(fallback) : std::string(value);
}

} // namespace

int main() {
    const std::string endpoint_secret = env_or("STRIPE_WEBHOOK_SECRET", "");
    if (endpoint_secret.empty()) {
        std::cerr << "Set STRIPE_WEBHOOK_SECRET to the whsec value printed by stripe listen.\n";
        return 1;
    }

    ::drogon::app().registerHandler("/webhook",
        [endpoint_secret](const ::drogon::HttpRequestPtr& request,
                          std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
            auto response = ::drogon::HttpResponse::newHttpResponse();
            try {
                const auto event = stripekit::webhooks::construct_event(
                    request->body(),
                    request->getHeader("stripe-signature"),
                    endpoint_secret);
                std::cout << "Verified event " << event.id << " of type " << event.type << '\n';
                response->setStatusCode(::drogon::k200OK);
                response->setBody(R"({"received":true})");
                response->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
            } catch (const stripekit::SignatureVerificationException& error) {
                std::cerr << "Webhook signature verification failed: " << error.what() << '\n';
                response->setStatusCode(::drogon::k400BadRequest);
            } catch (const std::exception& error) {
                std::cerr << "Webhook parsing failed: " << error.what() << '\n';
                response->setStatusCode(::drogon::k400BadRequest);
            }
            callback(response);
        }, {::drogon::Post});

    std::cout << "Webhook endpoint available at http://127.0.0.1:4242/webhook\n";
    ::drogon::app().addListener("127.0.0.1", 4242).run();
    return 0;
}