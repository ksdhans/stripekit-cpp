/** IntegrationTestServer.h
 * 
 * Local test server fixture using Drogon for StripeKit integration tests.
 * 
 * @author Hans de Ruiter
 * 
 * @license See LICENSE.md for details.
 */

#pragma once

#include "StripeKit/Adapters/Drogon.h"
#include "StripeKit/Async.h"
#include "StripeKit/Errors.h"
#include "StripeKit/Events.h"
#include "StripeKit/HTTPClient.h"
#include "StripeKit/Logging.h"
#include "StripeKit/Models.h"
#include "StripeKit/WebhookDedupStore.h"
#include "StripeKit/WebhookProcessor.h"
#include "StripeKit/Webhooks.h"

#include "TestFramework.h"

#include <drogon/drogon.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(STRIPEKIT_WITH_LIBSODIUM)
#include <sodium.h>
#else
#error "libsodium is required for integration test webhook verification"
#endif

namespace stripekit::testing {

/**
 * Encodes binary buffer into lowercase hexadecimal.
 */
inline std::string hex_encode(const unsigned char* data, std::size_t size) {
    static constexpr char hex_digits[] = "0123456789abcdef";
    std::string output;
    output.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        output[(i * 2)] = hex_digits[(data[i] >> 4) & 0x0F];
        output[(i * 2) + 1] = hex_digits[data[i] & 0x0F];
    }
    return output;
}

/**
 * Computes HMAC-SHA256 signature for signed webhook payload.
 */
inline std::string compute_hmac_sha256(std::string_view payload, std::string_view secret) {
    if (sodium_init() < 0) {
        throw std::runtime_error("sodium_init failed in compute_hmac_sha256");
    }
    unsigned char digest[crypto_auth_hmacsha256_BYTES] = {0};
    crypto_auth_hmacsha256(
        digest,
        reinterpret_cast<const unsigned char*>(payload.data()),
        payload.size(),
        reinterpret_cast<const unsigned char*>(secret.data()));
    return hex_encode(digest, crypto_auth_hmacsha256_BYTES);
}

/**
 * Constructs a valid Stripe-Signature header value.
 */
inline std::string make_stripe_signature_header(std::int64_t timestamp_unix,
                                                std::string_view payload,
                                                std::string_view secret) {
    std::ostringstream signed_payload;
    signed_payload << timestamp_unix << '.' << payload;
    const std::string signature = compute_hmac_sha256(signed_payload.str(), secret);
    return "t=" + std::to_string(timestamp_unix) + ",v1=" + signature;
}

/**
 * Mock EventDispatcher for verifying webhook signal dispatching.
 */
class MockEventDispatcher final : public EventDispatcher {
public:
    using DispatchHandler = std::function<EventDispatchResult(const VerifiedEventSignal&)>;

    void set_handler(DispatchHandler handler_fn) {
        std::lock_guard<std::mutex> lock(mutex);
        handler = std::move(handler_fn);
    }

    void set_result(EventDispatchResult result) {
        std::lock_guard<std::mutex> lock(mutex);
        default_result = result;
    }

    std::vector<VerifiedEventSignal> get_recorded_signals() const {
        std::lock_guard<std::mutex> lock(mutex);
        return recorded_signals;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        recorded_signals.clear();
    }

    EventDispatchResult dispatch(const VerifiedEventSignal& signal) override {
        std::lock_guard<std::mutex> lock(mutex);
        recorded_signals.push_back(signal);
        if (handler) {
            return handler(signal);
        }
        return default_result;
    }

private:
    mutable std::mutex mutex;
    std::vector<VerifiedEventSignal> recorded_signals;
    EventDispatchResult default_result = EventDispatchResult::Handled;
    DispatchHandler handler;
};

/**
 * Finds an available TCP port on localhost.
 */
inline std::uint16_t find_free_port() {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return 18080;
    }

    int opt = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sock);
        return 18080;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        ::close(sock);
        return 18080;
    }

    std::uint16_t port = ntohs(addr.sin_port);
    ::close(sock);
    return port;
}

/**
 * Shared state and telemetry for webhook handlers in integration tests.
 */
struct WebhookTestTelemetry {
    std::atomic<int> handled_events_count{0};
    std::atomic<int> ignored_events_count{0};
    std::vector<std::string> received_event_ids;
    std::mutex mutex;

    void record_event(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex);
        received_event_ids.push_back(id);
    }

    void reset() {
        handled_events_count = 0;
        ignored_events_count = 0;
        std::lock_guard<std::mutex> lock(mutex);
        received_event_ids.clear();
    }
};

/**
 * Global test server fixture that hosts the Drogon HTTP mock server and webhook endpoints.
 */
class DrogonIntegrationEnvironment {
public:
    static DrogonIntegrationEnvironment& get() {
        static DrogonIntegrationEnvironment env;
        return env;
    }

    static constexpr const char* webhook_secret = "whsec_integration_test_secret_abc123";
    static constexpr const char* api_key = "sk_test_integration_valid_key_456";

    InMemoryWebhookDedupStore dedup_store;
    WebhookTestTelemetry telemetry;
    std::unique_ptr<MockEventDispatcher> class_endpoint_dispatcher;
    std::unique_ptr<WebhookProcessor> class_endpoint_processor;
    std::unique_ptr<stripekit::drogon::WebhookEndpoint> class_endpoint;
    std::uint16_t server_port = 0;
    std::string base_url;

    void start() {
        if (running) {
            return;
        }

        server_port = find_free_port();
        base_url = "http://127.0.0.1:" + std::to_string(server_port);

        ::drogon::app().setLogLevel(trantor::Logger::kError);

        register_mock_stripe_api_handlers();
        register_webhook_handlers();
        register_fault_injection_handlers();

        ::drogon::app().addListener("127.0.0.1", server_port);

        server_thread = std::thread([]() {
            ::drogon::app().run();
        });

        for (int i = 0; i < 200 && !::drogon::app().isRunning(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        running = true;
    }

    void stop() {
        if (running && ::drogon::app().isRunning()) {
            ::drogon::app().quit();
            if (server_thread.joinable()) {
                server_thread.join();
            }
            running = false;
        }
    }

    /**
     * Sends a raw HTTP POST to a path on this test server.
     */
    std::pair<::drogon::ReqResult, ::drogon::HttpResponsePtr> send_raw_post(
        std::string_view path,
        std::string_view body,
        const std::unordered_map<std::string, std::string>& headers = {}) {
        auto client = ::drogon::HttpClient::newHttpClient(base_url);
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Post);
        req->setPath(std::string(path));
        req->setBody(std::string(body));
        for (const auto& [key, value] : headers) {
            req->addHeader(key, value);
        }
        return client->sendRequest(req, 10.0);
    }

    /**
     * Sends a raw HTTP GET to a path on this test server.
     */
    std::pair<::drogon::ReqResult, ::drogon::HttpResponsePtr> send_raw_get(
        std::string_view path,
        const std::unordered_map<std::string, std::string>& headers = {},
        double timeout = 10.0) {
        auto client = ::drogon::HttpClient::newHttpClient(base_url);
        auto req = ::drogon::HttpRequest::newHttpRequest();
        req->setMethod(::drogon::Get);
        req->setPath(std::string(path));
        for (const auto& [key, value] : headers) {
            req->addHeader(key, value);
        }
        return client->sendRequest(req, timeout);
    }

private:
    std::thread server_thread;

    void register_mock_stripe_api_handlers() {
        // POST /v1/customers
        ::drogon::app().registerHandler(
            "/v1/customers",
            [this](const ::drogon::HttpRequestPtr& req,
                   std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
                if (req->getHeader("authorization") != "Bearer " + std::string(api_key)) {
                    auto resp = ::drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(::drogon::k401Unauthorized);
                    resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                    resp->setBody(R"({"error":{"type":"authentication_error","message":"Invalid API Key"}})");
                    callback(resp);
                    return;
                }

                auto resp = ::drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(::drogon::k200OK);
                resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                resp->setBody(R"({
                    "id": "cus_drogon_123",
                    "object": "customer",
                    "email": "integration@example.com",
                    "name": "Integration User",
                    "livemode": false,
                    "metadata": {
                        "source": "integration_test"
                    }
                })");
                callback(resp);
            }, {::drogon::Post});

        // GET /v1/customers/{customer_id}
        ::drogon::app().registerHandler(
            "/v1/customers/{1}",
            [this](const ::drogon::HttpRequestPtr& req,
                   std::function<void(const ::drogon::HttpResponsePtr&)>&& callback,
                   const std::string& customer_id) {
                if (req->getHeader("authorization") != "Bearer " + std::string(api_key)) {
                    auto resp = ::drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(::drogon::k401Unauthorized);
                    resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                    resp->setBody(R"({"error":{"type":"authentication_error","message":"Invalid API Key"}})");
                    callback(resp);
                    return;
                }

                auto resp = ::drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(::drogon::k200OK);
                resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                resp->setBody(R"({
                    "id": ")" + customer_id + R"(",
                    "object": "customer",
                    "email": "retrieved@example.com",
                    "name": "Retrieved Customer",
                    "livemode": false,
                    "metadata": {}
                })");
                callback(resp);
            }, {::drogon::Get});

        // POST /v1/checkout/sessions
        ::drogon::app().registerHandler(
            "/v1/checkout/sessions",
            [this](const ::drogon::HttpRequestPtr& req,
                   std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
                if (req->getHeader("authorization") != "Bearer " + std::string(api_key)) {
                    auto resp = ::drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(::drogon::k401Unauthorized);
                    resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                    resp->setBody(R"({"error":{"type":"authentication_error","message":"Invalid API Key"}})");
                    callback(resp);
                    return;
                }

                auto resp = ::drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(::drogon::k200OK);
                resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                resp->setBody(R"({
                    "id": "cs_drogon_session_1",
                    "object": "checkout.session",
                    "customer": "cus_drogon_123",
                    "status": "open",
                    "mode": "subscription",
                    "url": "https://checkout.stripe.com/c/pay/cs_drogon_session_1",
                    "currency": "usd",
                    "amount_total": 2000,
                    "livemode": false,
                    "metadata": {}
                })");
                callback(resp);
            }, {::drogon::Post});

        // GET /v1/subscriptions/{subscription_id}
        ::drogon::app().registerHandler(
            "/v1/subscriptions/{1}",
            [this](const ::drogon::HttpRequestPtr& req,
                   std::function<void(const ::drogon::HttpResponsePtr&)>&& callback,
                   const std::string& subscription_id) {
                if (req->getHeader("authorization") != "Bearer " + std::string(api_key)) {
                    auto resp = ::drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(::drogon::k401Unauthorized);
                    resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                    resp->setBody(R"({"error":{"type":"authentication_error","message":"Invalid API Key"}})");
                    callback(resp);
                    return;
                }

                auto resp = ::drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(::drogon::k200OK);
                resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                resp->setBody(R"({
                    "id": ")" + subscription_id + R"(",
                    "object": "subscription",
                    "customer": "cus_drogon_123",
                    "status": "active",
                    "current_period_start": 1700000000,
                    "current_period_end": 1702592000,
                    "cancel_at_period_end": false,
                    "livemode": false,
                    "metadata": {}
                })");
                callback(resp);
            }, {::drogon::Get});

        // DELETE /v1/subscriptions/{subscription_id}
        ::drogon::app().registerHandler(
            "/v1/subscriptions/{1}",
            [this](const ::drogon::HttpRequestPtr& req,
                   std::function<void(const ::drogon::HttpResponsePtr&)>&& callback,
                   const std::string& subscription_id) {
                if (req->getHeader("authorization") != "Bearer " + std::string(api_key)) {
                    auto resp = ::drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(::drogon::k401Unauthorized);
                    resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                    resp->setBody(R"({"error":{"type":"authentication_error","message":"Invalid API Key"}})");
                    callback(resp);
                    return;
                }

                auto resp = ::drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(::drogon::k200OK);
                resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                resp->setBody(R"({
                    "id": ")" + subscription_id + R"(",
                    "object": "subscription",
                    "customer": "cus_drogon_123",
                    "status": "canceled",
                    "current_period_start": 1700000000,
                    "current_period_end": 1702592000,
                    "cancel_at_period_end": false,
                    "livemode": false,
                    "metadata": {}
                })");
                callback(resp);
            }, {::drogon::Delete});

        // POST /v1/billing_portal/sessions
        ::drogon::app().registerHandler(
            "/v1/billing_portal/sessions",
            [this](const ::drogon::HttpRequestPtr& req,
                   std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
                if (req->getHeader("authorization") != "Bearer " + std::string(api_key)) {
                    auto resp = ::drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(::drogon::k401Unauthorized);
                    resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                    resp->setBody(R"({"error":{"type":"authentication_error","message":"Invalid API Key"}})");
                    callback(resp);
                    return;
                }

                auto resp = ::drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(::drogon::k200OK);
                resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                resp->setBody(R"({
                    "id": "bps_drogon_session_1",
                    "object": "billing_portal.session",
                    "customer": "cus_drogon_123",
                    "url": "https://billing.stripe.com/session/bps_drogon_session_1",
                    "return_url": "https://example.com/account",
                    "created": 1700000000,
                    "livemode": false
                })");
                callback(resp);
            }, {::drogon::Post});
    }

    void register_fault_injection_handlers() {
        // 400 Bad Request
        ::drogon::app().registerHandler(
            "/fault/400-bad-request",
            [](const ::drogon::HttpRequestPtr&,
               std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
                auto resp = ::drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(::drogon::k400BadRequest);
                resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                resp->addHeader("request-id", "req_fault_400");
                resp->setBody(R"({"error":{"type":"invalid_request_error","message":"Missing required param: email"}})");
                callback(resp);
            }, {::drogon::Get, ::drogon::Post});

        // 402 Card Error
        ::drogon::app().registerHandler(
            "/fault/402-card-declined",
            [](const ::drogon::HttpRequestPtr&,
               std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
                auto resp = ::drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(static_cast<::drogon::HttpStatusCode>(402));
                resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                resp->addHeader("request-id", "req_fault_402");
                resp->setBody(R"({"error":{"type":"card_error","code":"card_declined","message":"Card declined"}})");
                callback(resp);
            }, {::drogon::Get, ::drogon::Post});

        // 429 Rate Limit
        ::drogon::app().registerHandler(
            "/fault/429-rate-limit",
            [](const ::drogon::HttpRequestPtr&,
               std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
                auto resp = ::drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(static_cast<::drogon::HttpStatusCode>(429));
                resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                resp->addHeader("request-id", "req_fault_429");
                resp->setBody(R"({"error":{"type":"rate_limit_error","message":"Too many requests"}})");
                callback(resp);
            }, {::drogon::Get, ::drogon::Post});

        // 500 Internal Server Error
        ::drogon::app().registerHandler(
            "/fault/500-server-error",
            [](const ::drogon::HttpRequestPtr&,
               std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
                auto resp = ::drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(::drogon::k500InternalServerError);
                resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                resp->addHeader("request-id", "req_fault_500");
                resp->setBody(R"({"error":{"type":"api_error","message":"Internal error in payment processing"}})");
                callback(resp);
            }, {::drogon::Get, ::drogon::Post});

        // 503 Service Unavailable
        ::drogon::app().registerHandler(
            "/fault/503-unavailable",
            [](const ::drogon::HttpRequestPtr&,
               std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
                auto resp = ::drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(::drogon::k503ServiceUnavailable);
                resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                resp->addHeader("request-id", "req_fault_503");
                resp->setBody(R"({"error":{"type":"api_connection_error","message":"Payment service temporarily down"}})");
                callback(resp);
            }, {::drogon::Get, ::drogon::Post});

        // Malformed non-JSON response
        ::drogon::app().registerHandler(
            "/fault/malformed-non-json",
            [](const ::drogon::HttpRequestPtr&,
               std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
                auto resp = ::drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(::drogon::k200OK);
                resp->setContentTypeCode(::drogon::CT_TEXT_PLAIN);
                resp->setBody("This is raw unformatted text, definitely not JSON!");
                callback(resp);
            }, {::drogon::Get, ::drogon::Post});

        // Slow response to trigger client timeout
        ::drogon::app().registerHandler(
            "/fault/timeout-delay",
            [](const ::drogon::HttpRequestPtr&,
               std::function<void(const ::drogon::HttpResponsePtr&)>&& callback) {
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
                auto resp = ::drogon::HttpResponse::newHttpResponse();
                resp->setStatusCode(::drogon::k200OK);
                resp->setContentTypeCode(::drogon::CT_APPLICATION_JSON);
                resp->setBody(R"({"status":"completed_after_delay"})");
                callback(resp);
            }, {::drogon::Get, ::drogon::Post});
    }

    void register_webhook_handlers() {
        // Standard webhook handler registered via Drogon adapter
        stripekit::drogon::register_webhook_handler({
            .path = "/test/webhook/standard",
            .endpoint_secret = webhook_secret,
            .dedup_store = dedup_store,
            .handler = [this](const stripekit::webhooks::Event& event)
                -> stripekit::Task<stripekit::webhooks::ProcessResult> {
                telemetry.record_event(event.id);
                if (event.type == "customer.created") {
                    telemetry.ignored_events_count++;
                    co_return stripekit::webhooks::ProcessResult::Ignored;
                }
                telemetry.handled_events_count++;
                co_return stripekit::webhooks::ProcessResult::Handled;
            }
        });

        // Webhook with retryable failure
        stripekit::drogon::register_webhook_handler({
            .path = "/test/webhook/retryable",
            .endpoint_secret = webhook_secret,
            .dedup_store = dedup_store,
            .handler = [](const stripekit::webhooks::Event&)
                -> stripekit::Task<stripekit::webhooks::ProcessResult> {
                co_return stripekit::webhooks::ProcessResult::RetryableFailure;
            }
        });

        // Webhook with fatal failure
        stripekit::drogon::register_webhook_handler({
            .path = "/test/webhook/fatal",
            .endpoint_secret = webhook_secret,
            .dedup_store = dedup_store,
            .handler = [](const stripekit::webhooks::Event&)
                -> stripekit::Task<stripekit::webhooks::ProcessResult> {
                co_return stripekit::webhooks::ProcessResult::FatalFailure;
            }
        });

        // Webhook with throwing exception
        stripekit::drogon::register_webhook_handler({
            .path = "/test/webhook/throwing",
            .endpoint_secret = webhook_secret,
            .dedup_store = dedup_store,
            .handler = [](const stripekit::webhooks::Event&)
                -> stripekit::Task<stripekit::webhooks::ProcessResult> {
                throw std::runtime_error("Unexpected database deadlock during webhook handling");
                co_return stripekit::webhooks::ProcessResult::Handled;
            }
        });

        // Webhook registered via WebhookEndpoint class wrapper
        class_endpoint_dispatcher = std::make_unique<MockEventDispatcher>();
        class_endpoint_processor = std::make_unique<WebhookProcessor>(
            dedup_store,
            *class_endpoint_dispatcher);
        class_endpoint = std::make_unique<stripekit::drogon::WebhookEndpoint>(
            *class_endpoint_processor,
            webhook_secret);
        class_endpoint->register_handler("/test/webhook/class-endpoint");
    }

    bool running = false;
};

} // namespace stripekit::testing
