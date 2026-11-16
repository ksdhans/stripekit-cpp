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
#include <drogon/utils/Utilities.h>
#include <drogon/utils/coroutine.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view stripe_api_version = stripekit::default_stripe_api_version;
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
    bool cli_starting = false;
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

/** Waits briefly for a signing secret without blocking other HTTP requests. */
::drogon::Task<std::string> wait_for_webhook_secret(std::string path) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const auto secret = extract_webhook_secret(read_file(path));
        if (!secret.empty()) {
            co_return secret;
        }
        co_await ::drogon::sleepCoro(::trantor::EventLoop::getEventLoopOfCurrentThread(),
                                     std::chrono::milliseconds(100));
    }
    co_return std::string();
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
        data.cli_starting = state->cli_starting;
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

/** Redirects a completed form submission to the home page with a notice. */
::drogon::HttpResponsePtr home_redirect(const std::string& notice) {
    auto response = ::drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(::drogon::k303SeeOther);
    response->addHeader("Location", "/?notice=" + ::drogon::utils::urlEncodeComponent(notice));
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
    return std::string(prefix) + "_" + ::drogon::utils::getUuid();
}

/** Creates a short-lived Stripe client for requests handled by the Drogon app. */
std::shared_ptr<stripekit::StripeClient> make_client(std::string api_key) {
    std::unique_ptr<stripekit::StripeClient> client = stripekit::drogon::make_stripe_client(
        { .api_key = std::move(api_key),
          .api_base_url = stripekit::default_api_base_url,
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
    view_data.insert("message", stripekit::redact_sensitive_text(error.what()));
    auto response = ::drogon::HttpResponse::newHttpViewResponse("ErrorPage", view_data);
    response->setStatusCode(::drogon::k500InternalServerError);
    return response;
}

/** Formats a Stripe timestamp as a UTC date and time. */
std::string format_event_date(std::int64_t unix_seconds) {
    const auto time = static_cast<std::time_t>(unix_seconds);
    std::tm utc{};
    if (::gmtime_r(&time, &utc) == nullptr)
        throw std::runtime_error("Could not format the webhook date");
    std::ostringstream text;
    text << std::put_time(&utc, "%Y-%m-%d %H:%M:%S UTC");
    return text.str();
}

/** Summarizes fields present in a verified event snapshot, without fetching current billing state.
 */
EventRecord make_event_record(const stripekit::webhooks::Event& event) {
    Json::Value payload;
    Json::CharReaderBuilder reader;
    std::istringstream input(event.payload_json);
    std::string errors;
    if (!Json::parseFromStream(reader, input, &payload, &errors))
        throw std::runtime_error("Could not parse verified event JSON: " + errors);

    EventRecord record{ .id = event.id,
                        .type = event.type,
                        .api_version = event.api_version,
                        .livemode = event.livemode };
    const auto& object = payload["data"]["object"];
    if (object.isObject()) {
        for (const auto& [field, label] :
             { std::pair{ "id", "Object ID" },
               { "customer", "Customer" },
               { "subscription", "Subscription" },
               { "payment_intent", "Payment intent" },
               { "status", "Status" },
               { "payment_status", "Payment status" },
               { "trial_start", "Trial start" },
               { "trial_end", "Trial end" },
               { "cancel_at_period_end", "Cancel at period end" },
               { "amount", "Amount (smallest currency unit)" },
               { "amount_total", "Total (smallest currency unit)" },
               { "amount_due", "Due (smallest currency unit)" },
               { "amount_paid", "Paid (smallest currency unit)" },
               { "amount_remaining", "Remaining (smallest currency unit)" },
               { "currency", "Currency" } }) {
            const auto& value = object[field];
            if ((std::string_view(field) == "trial_start" ||
                 std::string_view(field) == "trial_end") &&
                value.isInt64())
                record.details.emplace_back(label, format_event_date(value.asInt64()));
            else if (value.isString() || value.isNumeric() || value.isBool())
                record.details.emplace_back(label, value.asString());
            else if (value.isObject() && value["id"].isString())
                record.details.emplace_back(label, value["id"].asString());
        }
        const auto& parent = object["parent"];
        if (object["subscription"].isNull() && parent.isObject() &&
            parent["subscription_details"].isObject()) {
            const auto& subscription = parent["subscription_details"]["subscription"];
            if (subscription.isString())
                record.details.emplace_back("Subscription", subscription.asString());
            else if (subscription.isObject() && subscription["id"].isString())
                record.details.emplace_back("Subscription", subscription["id"].asString());
        }
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    record.json = Json::writeString(writer, payload);
    return record;
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
    if (!state->api_key.empty() && !state->api_key.starts_with("sk_test_")) {
        std::cerr << "This example requires a test-mode sk_test_ secret key.\n";
        return 1;
    }

    // Handler to show the current setup, checkout form, and recent verified webhook events.
    ::drogon::app().registerHandler(
        "/",
        [state, port](const ::drogon::HttpRequestPtr& request,
                      std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
            callback(home_response(state, port, request->getParameter("notice")));
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

            std::optional<std::int64_t> parsed_amount;
            const auto unit_amount = parameter_or(request, "unit_amount");
            if (!unit_amount.empty()) {
                try {
                    std::size_t parsed_length = 0;
                    parsed_amount = std::stoll(unit_amount, &parsed_length);
                    if (parsed_length != unit_amount.size() || *parsed_amount <= 0) {
                        throw std::invalid_argument("unit amount must be a positive integer");
                    }
                } catch (const std::invalid_argument&) {
                    callback(home_response(state,
                                           port,
                                           "Unit amount must be a positive integer.",
                                           ::drogon::k400BadRequest));
                    return;
                } catch (const std::out_of_range&) {
                    callback(home_response(state,
                                           port,
                                           "Unit amount must be a positive integer.",
                                           ::drogon::k400BadRequest));
                    return;
                }
            }
            const std::string webhook_secret = parameter_or(request, "webhook_secret");
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!api_key.empty()) {
                    state->api_key = api_key;
                }
                state->price_id = parameter_or(request, "price_id", state->price_id);
                state->product_name = parameter_or(request, "product_name", state->product_name);
                state->currency = parameter_or(request, "currency", state->currency);
                if (parsed_amount) {
                    state->unit_amount = *parsed_amount;
                }
                if (!webhook_secret.empty()) {
                    state->webhook_secret = webhook_secret;
                }
            }
            callback(home_redirect("Setup saved."));
        },
        { ::drogon::Post });

    // Starts stripe listen in the background and captures the webhook secret it prints.
    ::drogon::app().registerHandler(
        "/stripe-cli/start",
        [state, port](::drogon::HttpRequestPtr) -> ::drogon::Task<::drogon::HttpResponsePtr> {
            if (!stripe_cli_installed()) {
                co_return home_response(state,
                                        port,
                                        "Stripe CLI is not installed or not on PATH.",
                                        ::drogon::k400BadRequest);
            }
            bool already_starting;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                already_starting = state->cli_starting;
                if (!already_starting) {
                    state->cli_starting = true;
                }
            }
            if (already_starting) {
                co_return home_redirect("Stripe CLI listener startup is already in progress.");
            }
            std::ostringstream command;
            std::remove(std::string(listener_log_filename).c_str());
            command << "nohup stripe listen --all-snapshot --latest --forward-to http://127.0.0.1:"
                    << port << "/webhook > " << listener_log_filename << " 2>&1 &";
            const int result = std::system(command.str().c_str());
            std::string captured_secret;
            if (result == 0) {
                captured_secret =
                    co_await wait_for_webhook_secret(std::string(listener_log_filename));
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->cli_starting = false;
                state->cli_started = result == 0;
                if (!captured_secret.empty()) {
                    state->webhook_secret = captured_secret;
                }
            }
            co_return home_redirect(
                result != 0 ? "Could not start Stripe CLI listener."
                : captured_secret.empty()
                    ? "Stripe CLI listener start requested. Could not capture the "
                      "webhook secret yet; check stripe-listener.log."
                    : "Stripe CLI listener started and webhook secret captured.");
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

            const auto email = parameter_or(request, "email");
            const auto trial_choice = parameter_or(request, "trial", "0");
            if (email.empty() || (trial_choice != "0" && trial_choice != "14")) {
                co_return home_response(
                    state,
                    port,
                    "Enter an email and choose paid Checkout or a 14-day trial.",
                    ::drogon::k400BadRequest);
            }
            try {
                auto client = make_client(api_key);
                const stripekit::CreateCustomerRequest customer_request{
                    .email = email, .name = "StripeKit demo customer"
                };
                const auto customer = co_await client->customers().create_async(
                    customer_request, api_options(idempotency_key("customer")));

                const std::string domain = "http://127.0.0.1:" + std::to_string(port);
                stripekit::CreateCheckoutSessionRequest session_request{
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
                if (trial_choice == "14") {
                    session_request.trial_period_days = 14;
                    session_request.payment_method_collection = "always";
                    session_request.payment_method_types = { "card" };
                }
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
        [state](::drogon::HttpRequestPtr request) -> ::drogon::Task<::drogon::HttpResponsePtr> {
            std::string webhook_secret;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                webhook_secret = state->webhook_secret;
            }

            auto response = ::drogon::HttpResponse::newHttpResponse();
            if (webhook_secret.empty()) {
                response->setStatusCode(::drogon::k400BadRequest);
                co_return response;
            }

            try {
                const auto result = co_await stripekit::webhooks::process_event_once(
                    request->body(),
                    request->getHeader("stripe-signature"),
                    webhook_secret,
                    state->dedup_store,
                    // This handler runs only after StripeKit verifies the signature
                    // and claims the event id in the de-duplication store.
                    [state](const stripekit::webhooks::Event& event)
                        -> stripekit::Task<stripekit::webhooks::ProcessResult> {
                        auto record = make_event_record(event);
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->events.push_back(std::move(record));
                            if (state->events.size() > 20) {
                                state->events.erase(
                                    state->events.begin(),
                                    state->events.begin() +
                                        static_cast<std::ptrdiff_t>(state->events.size() - 20));
                            }
                        }
                        co_return stripekit::webhooks::ProcessResult::Handled;
                    });
                response->setStatusCode(result.response_action ==
                                                stripekit::WebhookResponseAction::AcknowledgeSuccess
                                            ? ::drogon::k200OK
                                            : ::drogon::k500InternalServerError);
                if (result.response_action != stripekit::WebhookResponseAction::AcknowledgeSuccess)
                    std::cerr << "Webhook handling failed; Stripe should retry event "
                              << result.event_id << '\n';
            } catch (const stripekit::SignatureVerificationException&) {
                response->setStatusCode(::drogon::k400BadRequest);
            } catch (const std::exception& error) {
                std::cerr << "Webhook failed: " << stripekit::redact_sensitive_text(error.what())
                          << '\n';
                response->setStatusCode(::drogon::k500InternalServerError);
            }
            co_return response;
        },
        { ::drogon::Post });

    std::cout << "StripeKit Drogon Checkout demo available at http://127.0.0.1:" << port << '\n';
    // addListener() binds the HTTP server, and run() starts Drogon's event
    // loops. The /checkout coroutine above and StripeKit's outbound HTTP client
    // both make progress on those loops.
    ::drogon::app().addListener("127.0.0.1", port).run();
    return 0;
}