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

/**
 * @addtogroup drogon_adapter
 * @{
 */

/**
 * Chooses which event loop outbound Stripe requests run on.
 *
 * Use drogon_framework() inside a Drogon server so StripeKit shares the loop
 * the application is already running, rather than starting a second one that
 * competes with it. The default, own(), suits command-line tools and tests
 * where there is no Drogon server.
 */
struct EventLoopOptions {
    /** Which loop to use. */
    enum class Mode {
        Own,             /**< StripeKit starts and owns a loop thread. */
        DrogonFramework, /**< Use the loop of the running Drogon application. */
        External         /**< Use a loop the application already owns. */
    };

    /**
     * Runs requests on a loop StripeKit starts and owns.
     *
     * @return Options selecting Mode::Own.
     */
    static EventLoopOptions own();

    /**
     * Runs requests on the loop of the running Drogon application.
     *
     * Requires Drogon to be running; use this from inside a Drogon server.
     *
     * @return Options selecting Mode::DrogonFramework.
     */
    static EventLoopOptions drogon_framework();

    /**
     * Runs requests on a loop the application already owns.
     *
     * @param loop The loop to use. It must outlive the HTTP client.
     * @return Options selecting Mode::External.
     */
    static EventLoopOptions external(trantor::EventLoop& loop);

    /** Which loop to use. */
    Mode mode = Mode::Own;

    /** The loop to use when #mode is Mode::External. Ignored otherwise. */
    trantor::EventLoop* loop = nullptr;
};

/** Settings for make_stripe_client(). */
struct ClientOptions {
    /**
     * Stripe secret key, of the form "sk_test_..." or "sk_live_...".
     *
     * Read it from configuration or the environment, never from source, and
     * never log it.
     */
    std::string api_key;

    /** Stripe API base URL. Override only to point tests at a mock server. */
    std::string api_base_url = "https://api.stripe.com";

    /** Which event loop outbound requests run on. */
    EventLoopOptions event_loop;
};

/** Settings for register_webhook_handler(). */
struct WebhookHandlerOptions {
    /** Route to register, for example "/stripe/webhook". */
    std::string path;

    /**
     * Signing secret for this endpoint, of the form "whsec_...".
     *
     * The Stripe CLI prints one for local testing; a registered endpoint has
     * its own, shown in the Stripe dashboard.
     */
    std::string endpoint_secret;

    /**
     * Where processed event ids are recorded, so each event runs once.
     *
     * Must outlive the handler, so use an application-scoped or `static`
     * object rather than a local.
     */
    WebhookDedupStore& dedup_store;

    /** Called once per event, after verification and de-duplication. */
    webhooks::EventHandler handler;

    /** Where to report failures. No logging when null. */
    Logger* logger = nullptr;

    /** How far the signature's timestamp may be from the current time. */
    std::chrono::seconds tolerance = std::chrono::minutes(5);

    /** How long before a claim left behind by a crashed process is reclaimed. */
    std::chrono::minutes stale_inflight_timeout = defaultStaleInflightTimeout;
};

/**
 * Sends Stripe API requests through Drogon's HTTP client.
 *
 * Construct this directly only when supplying a custom
 * stripekit::StripeResponseParser; otherwise make_stripe_client() does it for
 * you.
 *
 * Safe to use from several threads.
 */
class HTTPClient final : public stripekit::HTTPClient {
public:
    /**
     * Creates a client for a Stripe account.
     *
     * @param api_key Stripe secret key. Sent as the bearer token on every
     *        request; never logged.
     * @param api_base_url Stripe API base URL. Override only to point tests at
     *        a mock server.
     * @param event_loop Which event loop requests run on. Inside a Drogon
     *        server, pass EventLoopOptions::drogon_framework().
     */
    explicit HTTPClient(std::string api_key,
                        std::string api_base_url = "https://api.stripe.com",
                        EventLoopOptions event_loop = {});

    /** Destroys the client, stopping its event loop thread if it owns one. */
    ~HTTPClient() override;

    HTTPClient(const HTTPClient&) = delete;
    HTTPClient& operator=(const HTTPClient&) = delete;

    /**
     * Sends a request to Stripe.
     *
     * @param request What to send, including the options to honour.
     * @return A task producing Stripe's response. An error status is reported
     *         in the response rather than thrown.
     * @throws std::exception Stripe could not be reached.
     */
    Task<HttpResponse> send_async(const HttpRequest& request) override;

    /**
     * Sends a request to Stripe, reporting the result through a callback.
     *
     * The callback runs exactly once, on the client's event loop.
     *
     * @param request What to send, including the options to honour.
     * @param completion Receives Stripe's response, or the transport failure.
     */
    void send(const HttpRequest& request, Completion completion) override;

private:
    std::string api_key;
    std::unique_ptr<trantor::EventLoopThread> event_loop_thread;
    ::drogon::HttpClientPtr client;
};

/**
 * A Drogon route that receives Stripe webhook deliveries.
 *
 * Construct this directly only when webhook handling is driven by a
 * stripekit::EventDispatcher; otherwise register_webhook_handler() is simpler.
 *
 * The processor is referenced, not owned, so it must outlive the endpoint.
 */
class WebhookEndpoint final {
public:
    /**
     * Creates an endpoint.
     *
     * @param processor Verifies, de-duplicates, and dispatches deliveries. Must
     *        outlive the endpoint.
     * @param endpoint_secret Signing secret for this endpoint.
     */
    WebhookEndpoint(WebhookProcessor& processor, std::string endpoint_secret);

    /**
     * Adds the route to the Drogon application.
     *
     * Call this before Drogon starts running.
     *
     * @param path Route to register, for example "/stripe/webhook".
     */
    void register_handler(std::string path) const;

private:
    WebhookProcessor& processor;
    std::string endpoint_secret;
};

/**
 * Sends StripeKit's log entries through Drogon's logging.
 *
 * Pass one to WebhookHandlerOptions::logger so StripeKit's diagnostics appear
 * alongside the rest of the application's output.
 */
class LogBridge final : public stripekit::Logger {
public:
    /**
     * Creates a bridge.
     *
     * @param threshold Lowest severity to pass on. Entries below it are dropped
     *        before reaching Drogon.
     */
    explicit LogBridge(LogLevel threshold = LogLevel::Info);

    /**
     * Reports whether entries at this level are wanted.
     *
     * @param level Severity being considered.
     * @return True when it is at or above the configured threshold.
     */
    bool should_log(LogLevel level) const override;

    /**
     * Writes one entry through Drogon's matching log level.
     *
     * @param level Severity of the entry.
     * @param message The text to record.
     * @param context Identifiers to include in the entry.
     */
    void log(LogLevel level, std::string_view message, const LogContext& context) override;

private:
    LogLevel threshold;
};

/**
 * Creates a Stripe client that talks through Drogon.
 *
 * This is the usual way to get a stripekit::StripeClient in a Drogon
 * application. One client is enough for the whole application: it is safe to
 * share across threads and reuses connections to Stripe.
 *
 * @code
 * auto stripe = stripekit::drogon::make_stripe_client({
 *     .api_key = std::getenv("STRIPE_SECRET_KEY")
 * });
 * @endcode
 *
 * @param options The API key, and optionally the base URL and event loop.
 * @return The client. It owns its transport, so keeping this alive is enough.
 * @throws ConfigurationException The API key is missing, or the requested event
 *         loop is not available.
 *
 * @sa @ref quickstart
 */
std::unique_ptr<StripeClient> make_stripe_client(ClientOptions options);

/**
 * Adds a Stripe webhook route to the Drogon application.
 *
 * The route verifies Stripe's signature, reserves the event id in the store,
 * and only then calls the handler, so it runs once per event however many
 * times Stripe delivers it. Replies 200 when handled or already handled, 500
 * when the handler reports a retryable failure, and 400 when the signature
 * does not check out.
 *
 * Call this before Drogon starts running.
 *
 * @code
 * static stripekit::SqliteWebhookDedupStore dedup_store("stripe-webhooks.sqlite3");
 *
 * stripekit::drogon::register_webhook_handler({
 *     .path            = "/stripe/webhook",
 *     .endpoint_secret = std::getenv("STRIPE_WEBHOOK_SECRET"),
 *     .dedup_store     = dedup_store,
 *     .handler = [](const stripekit::webhooks::Event& event)
 *         -> stripekit::Task<stripekit::webhooks::ProcessResult> {
 *         co_return stripekit::webhooks::ProcessResult::Handled;
 *     }
 * });
 * @endcode
 *
 * @param options The route, signing secret, store, and handler. The store must
 *        outlive the running application.
 *
 * @sa @ref quickstart
 */
void register_webhook_handler(WebhookHandlerOptions options);

/** @} */

} // namespace stripekit::drogon