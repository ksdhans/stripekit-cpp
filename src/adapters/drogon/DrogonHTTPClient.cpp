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
#include <trantor/net/EventLoopThread.h>
#include <trantor/utils/Utilities.h>

#include <condition_variable>
#include <coroutine>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>

namespace stripekit::drogon {

namespace {

void require_tls_for_https(std::string_view api_base_url) {
    if (api_base_url.starts_with("https://")) {
        if (trantor::utils::tlsBackend() == "None") {
            throw ConfigurationException("The Drogon HTTP client requires a Trantor TLS backend "
                                         "for HTTPS Stripe API requests");
        }
        return;
    }

    constexpr std::string_view http_prefix = "http://";
    if (!api_base_url.starts_with(http_prefix)) {
        throw ConfigurationException("The Drogon HTTP client requires an HTTPS API base URL");
    }

    const std::string_view authority = api_base_url.substr(http_prefix.size());
    const std::size_t authority_end = authority.find_first_of("/?#");
    const std::string_view host = authority.substr(0, authority_end);
    const bool local_test_host =
        authority.find('@') == std::string_view::npos &&
        (host == "localhost" || host.starts_with("localhost:") || host == "127.0.0.1" ||
         host.starts_with("127.0.0.1:") || host == "[::1]" || host.starts_with("[::1]:"));
    if (!local_test_host) {
        throw ConfigurationException(
            "The Drogon HTTP client requires HTTPS except for loopback test servers");
    }
}

trantor::EventLoop* select_event_loop(EventLoopOptions options,
                                      std::unique_ptr<trantor::EventLoopThread>& owned_thread) {
    switch (options.mode) {
    case EventLoopOptions::Mode::Own:
        owned_thread = std::make_unique<trantor::EventLoopThread>("StripeKitDrogonHTTPClient");
        owned_thread->run();
        return owned_thread->getLoop();
    case EventLoopOptions::Mode::DrogonFramework:
        return nullptr;
    case EventLoopOptions::Mode::External:
        if (options.loop == nullptr) {
            throw ConfigurationException("An external Drogon event loop is required");
        }
        return options.loop;
    }
    throw ConfigurationException("Unsupported Drogon event loop mode");
}

class OwnedStripeClient final : public StripeClient {
public:
    explicit OwnedStripeClient(ClientOptions options)
        : http_client(
              std::move(options.api_key), std::move(options.api_base_url), options.event_loop),
          client(http_client) {}

    /** Return the underlying client's catalog product operations. */
    Products& products() override {
        return client.products();
    }

    /** Return the underlying client's catalog price operations. */
    Prices& prices() override {
        return client.prices();
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

EventLoopOptions EventLoopOptions::own() {
    return {};
}

EventLoopOptions EventLoopOptions::drogon_framework() {
    return EventLoopOptions{ .mode = Mode::DrogonFramework };
}

EventLoopOptions EventLoopOptions::external(trantor::EventLoop& loop) {
    return EventLoopOptions{ .mode = Mode::External, .loop = &loop };
}

HTTPClient::HTTPClient(std::string api_key, std::string api_base_url, EventLoopOptions event_loop)
    : api_key(std::move(api_key)) {
    if (this->api_key.empty()) {
        throw ConfigurationException("A Stripe API key is required for the Drogon HTTP client");
    }
    require_tls_for_https(api_base_url);
    client = ::drogon::HttpClient::newHttpClient(api_base_url,
                                                 select_event_loop(event_loop, event_loop_thread));
}

HTTPClient::~HTTPClient() = default;

Task<HttpResponse> HTTPClient::send_async(const HttpRequest& request) {
    /** Keeps the transport callback independent from the coroutine frame. */
    struct State {
        std::mutex mutex;
        std::condition_variable callback_finished;
        std::variant<HttpResponse, std::exception_ptr> result{ HttpResponse{} };
        std::coroutine_handle<> handle;
        std::thread::id callback_thread;
        bool callback_running = false;
        bool cancelled = false;

        /** Cancels resumption and waits for a callback on another thread. */
        void cancel() {
            std::unique_lock<std::mutex> lock(mutex);
            cancelled = true;
            handle = {};
            if (callback_running && callback_thread != std::this_thread::get_id()) {
                callback_finished.wait(lock, [this]() { return !callback_running; });
            }
        }

        /** Stores the result and resumes the coroutine unless it was cancelled. */
        void complete(std::variant<HttpResponse, std::exception_ptr> value) {
            std::coroutine_handle<> handle_to_resume;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (cancelled) {
                    return;
                }
                result = std::move(value);
                callback_running = true;
                callback_thread = std::this_thread::get_id();
                handle_to_resume = handle;
            }

            handle_to_resume.resume();

            {
                std::lock_guard<std::mutex> lock(mutex);
                callback_running = false;
                callback_finished.notify_all();
            }
        }
    };

    struct Awaiter {
        HTTPClient& http_client;
        HttpRequest request;
        std::shared_ptr<State> state;

        ~Awaiter() {
            state->cancel();
        }

        bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->handle = handle;
            }
            try {
                http_client.send(
                    request,
                    [state = state](std::variant<HttpResponse, std::exception_ptr> value) mutable {
                        state->complete(std::move(value));
                    });
            } catch (...) {
                state->complete(std::current_exception());
            }
        }

        HttpResponse await_resume() {
            if (const auto* error = std::get_if<std::exception_ptr>(&state->result)) {
                std::rethrow_exception(*error);
            }
            return std::get<HttpResponse>(std::move(state->result));
        }
    };

    // Awaiting through a named local instead of `co_return co_await Awaiter{...}`
    // directly: GCC 12 miscalculates the coroutine frame size for the combined
    // pattern, corrupting the heap once the network callback writes into it.
    Awaiter awaiter{ *this, request, std::make_shared<State>() };
    HttpResponse response = co_await awaiter;
    co_return response;
}

void HTTPClient::send(const HttpRequest& request, Completion completion) {
    auto once_completion =
        std::make_shared<OnceCallback<std::variant<HttpResponse, std::exception_ptr>>>(
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
    http_request->setPathEncode(false);
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
        HttpResponse converted{ .status_code = static_cast<int>(response->statusCode()),
                                .body = std::string(response->body()) };
        for (const auto& [name, value] : response->headers()) {
            converted.headers.emplace(name, value);
        }
        once_completion->invoke(std::move(converted));
    };

    if (request.options.timeout.has_value()) {
        client->sendRequest(
            http_request, callback, static_cast<double>(request.options.timeout->count()) / 1000.0);
    } else {
        client->sendRequest(http_request, callback);
    }
}

std::unique_ptr<StripeClient> make_stripe_client(ClientOptions options) {
    return std::make_unique<OwnedStripeClient>(std::move(options));
}

} // namespace stripekit::drogon