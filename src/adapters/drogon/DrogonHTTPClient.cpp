/** DrogonHTTPClient.cpp
 *
 * See header file for details.
 */

#include "StripeKit/Adapters/Drogon.h"

#include "StripeKit/Callback.h"
#include "StripeKit/Errors.h"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <future>
#include <utility>

namespace stripekit::drogon {

namespace {

class OwnedStripeClient final : public StripeClient {
public:
    explicit OwnedStripeClient(ClientOptions options)
        : http_client(std::move(options.api_key), std::move(options.api_base_url)),
          client(http_client) {
    }

    Customers& customers() override {
        return client.customers();
    }

    Checkout& checkout() override {
        return client.checkout();
    }

    Subscriptions& subscriptions() override {
        return client.subscriptions();
    }

    BillingPortal& billing_portal() override {
        return client.billing_portal();
    }

private:
    HTTPClient http_client;
    StripeApiClient client;
};

} // namespace

HTTPClient::HTTPClient(std::string api_key, std::string api_base_url)
    : api_key(std::move(api_key)), client(::drogon::HttpClient::newHttpClient(api_base_url)) {
    if (this->api_key.empty()) {
        throw ConfigurationException("A Stripe API key is required for the Drogon HTTP client");
    }
}

HTTPClient::~HTTPClient() = default;

Task<HttpResponse> HTTPClient::send_async(const HttpRequest& request) {
    auto promise = std::make_shared<std::promise<HttpResponse>>();
    auto future = promise->get_future();
    send(request, [promise](std::variant<HttpResponse, std::exception_ptr> result) {
        try {
            if (const auto* error = std::get_if<std::exception_ptr>(&result)) {
                std::rethrow_exception(*error);
            }
            promise->set_value(std::get<HttpResponse>(std::move(result)));
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    co_return future.get();
}

void HTTPClient::send(const HttpRequest& request, Completion completion) {
    auto once_completion = std::make_shared<OnceCallback<std::variant<HttpResponse, std::exception_ptr>>>(
        std::move(completion));
    auto http_request = ::drogon::HttpRequest::newHttpRequest();
    switch (request.method) {
    case HttpMethod::Get:
        http_request->setMethod(::drogon::Get);
        break;
    case HttpMethod::Post:
        http_request->setMethod(::drogon::Post);
        break;
    case HttpMethod::Delete:
        http_request->setMethod(::drogon::Delete);
        break;
    }
    http_request->setPath(request.path);
    http_request->addHeader("Authorization", "Bearer " + api_key);
    for (const auto& [name, value] : request.headers) {
        http_request->addHeader(name, value);
    }
    http_request->setBody(request.body);

    const auto callback = [once_completion](::drogon::ReqResult result,
                                            const ::drogon::HttpResponsePtr& response) {
        if (result != ::drogon::ReqResult::Ok || response == nullptr) {
            once_completion->invoke(std::make_exception_ptr(
                StripeKitException("Drogon could not complete the Stripe API request")));
            return;
        }
        HttpResponse converted{
            .status_code = static_cast<int>(response->statusCode()),
            .body = std::string(response->body())
        };
        for (const auto& [name, value] : response->headers()) {
            converted.headers.emplace(name, value);
        }
        once_completion->invoke(std::move(converted));
    };

    if (request.options.timeout.has_value()) {
        client->sendRequest(http_request, callback,
            static_cast<double>(request.options.timeout->count()) / 1000.0);
    } else {
        client->sendRequest(http_request, callback);
    }
}

std::unique_ptr<StripeClient> make_stripe_client(ClientOptions options) {
    return std::make_unique<OwnedStripeClient>(std::move(options));
}

} // namespace stripekit::drogon