/** Drogon.h
 *
 * Drogon runtime adapter for StripeKit outbound requests and webhooks.
 *
 * @author Hans de Ruiter
 *
 * @License See LICENSE for details.
 */

#pragma once

#include "StripeKit/HTTPClient.h"
#include "StripeKit/Logging.h"
#include "StripeKit/StripeApiClient.h"
#include "StripeKit/WebhookProcessor.h"
#include "StripeKit/Webhooks.h"

#include <drogon/HttpClient.h>

#include <chrono>
#include <memory>
#include <string>

namespace trantor {
class EventLoop;
class EventLoopThread;
}

namespace stripekit::drogon {

/** Selects the event loop used for outbound Stripe API requests. */
struct EventLoopOptions {
    /** Event loop ownership mode. */
    enum class Mode {
        Own,
        DrogonFramework,
        External
    };

    /** Uses a StripeKit-owned event loop thread. */
    static EventLoopOptions own();

    /** Uses Drogon's framework event loop. */
    static EventLoopOptions drogon_framework();

    /** Uses an application-owned event loop. */
    static EventLoopOptions external(trantor::EventLoop& loop);

    /** Selected ownership mode. */
    Mode mode = Mode::Own;

    /** Application-owned event loop when mode is External. */
    trantor::EventLoop* loop = nullptr;
};

/** Options for creating a Stripe API client backed by Drogon. */
struct ClientOptions {
    /** Stripe secret API key. */
    std::string api_key;

    /** Stripe API base URL. */
    std::string api_base_url = "https://api.stripe.com";

    /** Event loop configuration for outbound Stripe API requests. */
    EventLoopOptions event_loop;
};

/** Options for registering a Drogon webhook handler. */
struct WebhookHandlerOptions {
    /** Absolute Drogon route path, for example /stripe/webhook. */
    std::string path;

    /** Stripe webhook endpoint secret. */
    std::string endpoint_secret;

    /** Store used to process each Stripe event id once. */
    WebhookDedupStore& dedup_store;

    /** Application handler called for freshly claimed webhook events. */
    webhooks::EventHandler handler;

    /** Optional log sink. */
    Logger* logger = nullptr;

    /** Maximum allowed timestamp skew from Stripe's signature header. */
    std::chrono::seconds tolerance = std::chrono::minutes(5);

    /** Timeout after which an unfinished event claim may be reclaimed. */
    std::chrono::minutes stale_inflight_timeout = defaultStaleInflightTimeout;
};

/**
 * Sends Stripe API requests through Drogon's HTTP client.
 *
 * Most applications should use make_stripe_client instead of constructing this
 * type directly.
 */
class HTTPClient final : public stripekit::HTTPClient {
public:
    /** Creates a client for the supplied Stripe secret API key. */
    explicit HTTPClient(std::string api_key,
                        std::string api_base_url = "https://api.stripe.com",
                        EventLoopOptions event_loop = {});

    /** Destroys the client. */
    ~HTTPClient() override;

    HTTPClient(const HTTPClient&) = delete;
    HTTPClient& operator=(const HTTPClient&) = delete;

    /** Starts a coroutine that sends a Stripe API request. */
    Task<HttpResponse> send_async(const HttpRequest& request) override;

    /** Sends a request and invokes completion exactly once on the client event loop. */
    void send(const HttpRequest& request, Completion completion) override;

private:
    std::string api_key;
    std::unique_ptr<trantor::EventLoopThread> event_loop_thread;
    ::drogon::HttpClientPtr client;
};

/**
 * Registers a Stripe webhook endpoint with the Drogon application.
 *
 * Most applications should use register_webhook_handler instead of constructing
 * this type directly.
 */
class WebhookEndpoint final {
public:
    /** Creates an endpoint using an application-owned webhook processor. */
    WebhookEndpoint(WebhookProcessor& processor, std::string endpoint_secret);

    /** Registers this endpoint at an absolute Drogon route path. */
    void register_handler(std::string path) const;

private:
    WebhookProcessor& processor;
    std::string endpoint_secret;
};

/** Bridges StripeKit log entries into Drogon's native logging facilities. */
class LogBridge final : public stripekit::Logger {
public:
    /** Creates a logger with the provided minimum threshold. */
    explicit LogBridge(LogLevel threshold = LogLevel::Info);

    /** Returns whether a message at the given level will be emitted. */
    bool should_log(LogLevel level) const override;

    /** Writes an entry through Drogon's matching native log level. */
    void log(LogLevel level, std::string_view message, const LogContext& context) override;

private:
    LogLevel threshold;
};

/** Creates a Stripe API client backed by Drogon. */
std::unique_ptr<StripeClient> make_stripe_client(ClientOptions options);

/** Registers a Drogon webhook handler using StripeKit verification and de-duplication. */
void register_webhook_handler(WebhookHandlerOptions options);

} // namespace stripekit::drogon