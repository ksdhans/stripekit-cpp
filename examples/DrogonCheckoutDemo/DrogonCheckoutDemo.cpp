/** DrogonCheckoutDemo.cpp
 *
 * See README.md for details.
 */

#include "StripeKit/Adapters/Drogon.h"
#include "StripeKit/Client.h"
#include "StripeKit/Errors.h"
#include "StripeKit/WebhookDedupStore.h"

#include "CheckoutDemoViewData.h"

#include <drogon/drogon.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view stripe_api_version = "2024-06-20";
constexpr std::string_view listener_log_filename = "stripe-listener.log";

using stripekit::examples::CheckoutDemoViewData;
using stripekit::examples::EventRecord;
using stripekit::examples::ReturnPageViewData;

/** Shared in-memory demo configuration and webhook history. */
struct AppState {
    std::mutex mutex;
    std::string api_key;
    std::string price_id;
    std::string product_name = "StripeKit demo membership";
    std::string currency = "usd";
    std::int64_t unit_amount = 1200;
    std::string webhook_secret;
    bool cli_started = false;
    stripekit::InMemoryWebhookDedupStore dedup_store;
    std::vector<EventRecord> events;
};

/** Returns an environment variable, or a fallback when it is not set. */
std::string env_or(std::string_view name, std::string fallback) {
    const char* value = std::getenv(std::string(name).c_str());
    return value == nullptr || std::string_view(value).empty() ? std::move(fallback)
                                                               : std::string(value);
}

/** Reads the demo port from --port, STRIPEKIT_DEMO_PORT, or the default. */
std::uint16_t parse_port(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--port") {
            return static_cast<std::uint16_t>(std::stoi(argv[i + 1]));
        }
    }
    return static_cast<std::uint16_t>(std::stoi(env_or("STRIPEKIT_DEMO_PORT", "4242")));
}

/** Returns whether the Stripe CLI is available for local webhook forwarding. */
bool stripe_cli_installed() {
    return std::system("command -v stripe >/dev/null 2>&1") == 0;
}

/** Finds the first webhook signing secret in Stripe CLI listener output. */
std::string extract_webhook_secret(std::string_view text) {
    constexpr std::string_view prefix = "whsec_";
    const auto start = text.find(prefix);
    if (start == std::string_view::npos) {
        return {};
    }

    auto end = start;
    while (end < text.size()) {
        const unsigned char character = static_cast<unsigned char>(text[end]);
        if (std::isalnum(character) || text[end] == '_') {
            ++end;
        } else {
            break;
        }
    }
    return std::string(text.substr(start, end - start));
}

/** Reads a small text file and returns an empty string when it is unavailable. */
std::string read_file(std::string_view path) {
    std::ifstream input{ std::string(path) };
    if (!input) {
        return {};
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

/** Waits briefly for stripe listen to print a webhook signing secret. */
std::string wait_for_webhook_secret(std::string_view path) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto secret = extract_webhook_secret(read_file(path));
        if (!secret.empty()) {
            return secret;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return {};
}

/** Returns the absolute path to the Stripe CLI listener log file. */
std::string listener_log_path() {
    return std::filesystem::absolute(std::string(listener_log_filename)).string();
}

/** Summarizes whether the demo has enough credentials to run checkout. */
std::string configured_summary(std::string_view api_key, std::string_view webhook_secret) {
    if (api_key.empty()) {
        return "Secret key is not configured.";
    }
    if (webhook_secret.empty()) {
        return "Secret key configured. Webhook signing secret is not configured.";
    }
    return "Secret key and webhook signing secret are configured.";
}

/** Creates the view data for the checkout demo's home page. */
CheckoutDemoViewData make_home_view_data(const std::shared_ptr<AppState>& state,
                                         std::uint16_t port,
                                         std::string notice = {}) {
    CheckoutDemoViewData data;
    data.port = port;
    data.notice = std::move(notice);
    data.stripe_cli_installed = stripe_cli_installed();
    data.listener_log_path = listener_log_path();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        data.has_api_key = !state->api_key.empty();
        data.price_id = state->price_id;
        data.product_name = state->product_name;
        data.currency = state->currency;
        data.unit_amount = state->unit_amount;
        data.has_webhook_secret = !state->webhook_secret.empty();
        data.cli_started = state->cli_started;
        data.events = state->events;
    }
    data.configured_summary = configured_summary(data.has_api_key ? "sk_test_" : "",
                                                 data.has_webhook_secret ? "whsec_" : "");
    data.can_start_checkout = data.has_api_key;
    return data;
}

/** Home page HTTP response.
 *
 * @param state The shared application state.
 * @param port The port on which the server is running.
 * @param notice An optional notice message to display on the home page.
 * @param status The HTTP status code for the response.
 * @return The HTTP response for the home page.
 */
::drogon::HttpResponsePtr home_response(const std::shared_ptr<AppState>& state,
                                        std::uint16_t port,
                                        std::string notice = {},
                                        ::drogon::HttpStatusCode status = ::drogon::k200OK) {
    ::drogon::HttpViewData view_data;
    view_data.insert("data", make_home_view_data(state, port, std::move(notice)));
    auto response = ::drogon::HttpResponse::newHttpViewResponse("Home", view_data);
    response->setStatusCode(status);
    return response;
}

/** Returns a form/query parameter, or a fallback when the parameter is empty. */
std::string parameter_or(const ::drogon::HttpRequestPtr& request,
                         const std::string& name,
                         std::string fallback = {}) {
    const auto value = request->getParameter(name);
    return value.empty() ? std::move(fallback) : value;
}

/** Creates request options shared by the demo's Stripe API calls. */
stripekit::RequestOptions api_options(std::string idempotency_key = {}) {
    stripekit::RequestOptions options;
    options.headers.emplace("Stripe-Version", std::string(stripe_api_version));
    if (!idempotency_key.empty()) {
        options.idempotency_key = std::move(idempotency_key);
    }
    return options;
}

/** Generates a unique idempotency key with the supplied prefix. */
std::string idempotency_key(std::string_view prefix) {
    const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return std::string(prefix) + "_" + std::to_string(now);
}

/** Creates a short-lived Stripe client for requests handled by the Drogon app. */
std::shared_ptr<stripekit::StripeClient> make_client(std::string api_key) {
    std::unique_ptr<stripekit::StripeClient> client = stripekit::drogon::make_stripe_client(
        { .api_key = std::move(api_key),
          .api_base_url = "https://api.stripe.com",
          // This demo is already running inside Drogon, so StripeKit sends
          // outbound API requests through the Drogon framework's event loop instead
          // of creating a separate StripeKit-owned event loop thread.
          .event_loop = stripekit::drogon::EventLoopOptions::drogon_framework() });
    return std::shared_ptr<stripekit::StripeClient>(std::move(client));
}

/** Builds an error page response from an exception message. */
::drogon::HttpResponsePtr error_response(std::string_view title, const std::exception& error) {
    ::drogon::HttpViewData view_data;
    view_data.insert("title", std::string(title));
    view_data.insert("message", std::string(error.what()));
    auto response = ::drogon::HttpResponse::newHttpViewResponse("ErrorPage", view_data);
    response->setStatusCode(::drogon::k500InternalServerError);
    return response;
}

} // namespace

int main(int argc, char** argv) {
    const auto port = parse_port(argc, argv);
    const auto state = std::make_shared<AppState>();

    // Seed the setup form from environment variables, then keep edits in memory
    // for the lifetime of this demo process.
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->api_key = env_or("STRIPE_SECRET_KEY", "");
        state->price_id = env_or("STRIPE_PRICE_ID", "");
        state->webhook_secret = env_or("STRIPE_WEBHOOK_SECRET", "");
    }

    // Handler to show the current setup, checkout form, and recent verified webhook events.
    ::drogon::app().registerHandler(
        "/",
        [state, port](const ::drogon::HttpRequestPtr&,
                      std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
            callback(home_response(state, port));
        },
        { ::drogon::Get });

    // Handler to save test-mode Stripe settings submitted from the setup form.
    ::drogon::app().registerHandler(
        "/setup",
        [state, port](const ::drogon::HttpRequestPtr& request,
                      std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
            const std::string api_key = parameter_or(request, "api_key");
            if (api_key.starts_with("sk_live_")) {
                callback(home_response(state,
                                       port,
                                       "Live keys are rejected by this example.",
                                       ::drogon::k400BadRequest));
                return;
            }
            if (!api_key.empty() && !api_key.starts_with("sk_test_")) {
                callback(home_response(state,
                                       port,
                                       "Use a Stripe test-mode secret key beginning with sk_test_.",
                                       ::drogon::k400BadRequest));
                return;
            }

            std::lock_guard<std::mutex> lock(state->mutex);
            if (!api_key.empty()) {
                state->api_key = api_key;
            }
            state->price_id = parameter_or(request, "price_id", state->price_id);
            state->product_name = parameter_or(request, "product_name", state->product_name);
            state->currency = parameter_or(request, "currency", state->currency);
            const auto unit_amount = parameter_or(request, "unit_amount");
            if (!unit_amount.empty()) {
                try {
                    std::size_t parsed_length = 0;
                    const auto parsed_amount = std::stoll(unit_amount, &parsed_length);
                    if (parsed_length != unit_amount.size() || parsed_amount <= 0) {
                        throw std::invalid_argument("unit amount must be a positive integer");
                    }
                    state->unit_amount = parsed_amount;
                } catch (const std::exception&) {
                    callback(home_response(state,
                                           port,
                                           "Unit amount must be a positive integer.",
                                           ::drogon::k400BadRequest));
                    return;
                }
            }
            const std::string webhook_secret = parameter_or(request, "webhook_secret");
            if (!webhook_secret.empty()) {
                state->webhook_secret = webhook_secret;
            }
            callback(home_response(state, port, "Setup saved."));
        },
        { ::drogon::Post });

    // Starts stripe listen in the background and captures the webhook secret it prints.
    ::drogon::app().registerHandler(
        "/stripe-cli/start",
        [state, port](const ::drogon::HttpRequestPtr&,
                      std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
            if (!stripe_cli_installed()) {
                callback(home_response(state,
                                       port,
                                       "Stripe CLI is not installed or not on PATH.",
                                       ::drogon::k400BadRequest));
                return;
            }
            std::ostringstream command;
            std::remove(std::string(listener_log_filename).c_str());
            command << "nohup stripe listen --latest --forward-to http://127.0.0.1:" << port
                    << "/webhook > " << listener_log_filename << " 2>&1 &";
            const int result = std::system(command.str().c_str());
            const auto captured_secret =
                result == 0 ? wait_for_webhook_secret(listener_log_filename) : std::string();
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->cli_started = result == 0;
                if (!captured_secret.empty()) {
                    state->webhook_secret = captured_secret;
                }
            }
            callback(
                home_response(state,
                              port,
                              result != 0 ? "Could not start Stripe CLI listener."
                              : captured_secret.empty()
                                  ? "Stripe CLI listener start requested. Could not capture the "
                                    "webhook secret yet; check stripe-listener.log."
                                  : "Stripe CLI listener started and webhook secret captured."));
        },
        { ::drogon::Post });

    // Creates the Stripe customer and Checkout Session, then redirects the browser to Stripe.
    ::drogon::app().registerHandler(
        "/checkout",
        [state,
         port](::drogon::HttpRequestPtr request) -> ::drogon::Task<::drogon::HttpResponsePtr> {
            // This route is a Drogon coroutine. Because the Stripe client uses
            // Drogon's framework event loop, the handler must co_await async
            // StripeKit operations instead of blocking with Task::result().
            std::string api_key;
            std::string price_id;
            std::string product_name;
            std::string currency;
            std::int64_t unit_amount = 0;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                api_key = state->api_key;
                price_id = state->price_id;
                product_name = state->product_name;
                currency = state->currency;
                unit_amount = state->unit_amount;
            }
            if (api_key.empty()) {
                co_return home_response(
                    state, port, "Configure the secret key first.", ::drogon::k400BadRequest);
            }

            const std::string email = parameter_or(request, "email");
            auto client = make_client(api_key);
            try {
                // Create a test Customer first, then create a subscription-mode
                // Checkout Session for that Customer and redirect to Stripe.
                stripekit::CreateCustomerRequest customer_request = {
                    .email = email, .name = "StripeKit demo customer"
                };
                const auto customer = co_await client->customers().create_async(
                    customer_request, api_options(idempotency_key("customer")));

                const std::string domain = "http://127.0.0.1:" + std::to_string(port);
                stripekit::CreateCheckoutSessionRequest session_request = {
                    .customer_id = customer.id,
                    .price_id = price_id,
                    .product_name = product_name,
                    .currency = currency,
                    .unit_amount = unit_amount,
                    .recurring_interval = "month",
                    .success_url = domain + "/return?session_id={CHECKOUT_SESSION_ID}",
                    .cancel_url = domain + "/",
                    .quantity = 1,
                    .metadata = { { "example", "drogon-checkout-demo" } }
                };
                const auto session = co_await client->checkout().sessions().create_async(
                    session_request, api_options(idempotency_key("checkout")));

                auto response = ::drogon::HttpResponse::newHttpResponse();
                response->setStatusCode(::drogon::k303SeeOther);
                response->addHeader("Location", session.url);
                co_return response;
            } catch (const std::exception& exception) {
                co_return error_response("Could not create Checkout Session", exception);
            }
        },
        { ::drogon::Post });

    // Displays the session id that Stripe places in the success redirect URL.
    ::drogon::app().registerHandler(
        "/return",
        [](const ::drogon::HttpRequestPtr& request,
           std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
            ::drogon::HttpViewData view_data;
            view_data.insert(
                "data", ReturnPageViewData{ .session_id = request->getParameter("session_id") });
            callback(::drogon::HttpResponse::newHttpViewResponse("ReturnPage", view_data));
        },
        { ::drogon::Get });

    // Verifies webhook signatures, records fresh events, and acknowledges Stripe.
    ::drogon::app().registerHandler(
        "/webhook",
        [state](const ::drogon::HttpRequestPtr& request,
                std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
            std::string webhook_secret;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                webhook_secret = state->webhook_secret;
            }

            auto response = ::drogon::HttpResponse::newHttpResponse();
            if (webhook_secret.empty()) {
                response->setStatusCode(::drogon::k400BadRequest);
                callback(response);
                return;
            }

            try {
                auto task = stripekit::webhooks::process_event_once(
                    request->body(),
                    request->getHeader("stripe-signature"),
                    webhook_secret,
                    state->dedup_store,
                    // This handler runs only after StripeKit verifies the signature
                    // and claims the event id in the de-duplication store.
                    [state](const stripekit::webhooks::Event& event)
                        -> stripekit::Task<stripekit::webhooks::ProcessResult> {
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->events.push_back(EventRecord{ .id = event.id,
                                                                 .type = event.type,
                                                                 .api_version = event.api_version,
                                                                 .livemode = event.livemode });
                            if (state->events.size() > 20) {
                                state->events.erase(
                                    state->events.begin(),
                                    state->events.begin() +
                                        static_cast<std::ptrdiff_t>(state->events.size() - 20));
                            }
                        }
                        co_return stripekit::webhooks::ProcessResult::Handled;
                    });
                const auto result = task.result();
                response->setStatusCode(result.response_action ==
                                                stripekit::WebhookResponseAction::AcknowledgeSuccess
                                            ? ::drogon::k200OK
                                            : ::drogon::k500InternalServerError);
            } catch (const stripekit::SignatureVerificationException&) {
                response->setStatusCode(::drogon::k400BadRequest);
            } catch (const std::exception&) {
                response->setStatusCode(::drogon::k400BadRequest);
            }
            callback(response);
        },
        { ::drogon::Post });

    std::cout << "StripeKit Drogon Checkout demo available at http://127.0.0.1:" << port << '\n';
    // addListener() binds the HTTP server, and run() starts Drogon's event
    // loops. The /checkout coroutine above and StripeKit's outbound HTTP client
    // both make progress on those loops.
    ::drogon::app().addListener("127.0.0.1", port).run();
    return 0;
}