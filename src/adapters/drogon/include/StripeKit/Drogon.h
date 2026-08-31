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
#include "StripeKit/WebhookProcessor.h"

#include <drogon/HttpClient.h>

#include <string>

namespace stripekit {

/**
 * Sends Stripe API requests through Drogon's HTTP client.
 *
 * Callback completions run on the Drogon event loop used by the HTTP client.
 */
class DrogonHTTPClient final : public HTTPClient {
public:
    /** Creates a client for the supplied Stripe secret API key. */
    explicit DrogonHTTPClient(std::string api_key,
                              std::string api_base_url = "https://api.stripe.com");

    /** Destroys the client. */
    ~DrogonHTTPClient() override;

    DrogonHTTPClient(const DrogonHTTPClient&) = delete;
    DrogonHTTPClient& operator=(const DrogonHTTPClient&) = delete;

    /** Starts a coroutine that sends a Stripe API request. */
    Task<HttpResponse> send_async(const HttpRequest& request) override;

    /** Sends a request and invokes completion exactly once on Drogon's event loop. */
    void send(const HttpRequest& request, Completion completion) override;

private:
    std::string api_key;
    drogon::HttpClientPtr client;
};

/**
 * Registers a Stripe webhook endpoint with the Drogon application.
 *
 * The endpoint returns 400 for invalid signatures or event payloads, 200 for
 * successful and previously processed deliveries, and 500 when Stripe should retry.
 */
class DrogonWebhookEndpoint final {
public:
    /** Creates an endpoint using an application-owned webhook processor. */
    DrogonWebhookEndpoint(WebhookProcessor& processor, std::string endpoint_secret);

    /** Registers this endpoint at an absolute Drogon route path. */
    void register_handler(std::string path) const;

private:
    WebhookProcessor& processor;
    std::string endpoint_secret;
};

/**
 * Bridges StripeKit log entries into Drogon's native logging facilities.
 */
class DrogonLogger final : public Logger {
public:
    /** Creates a logger with the provided minimum threshold. */
    explicit DrogonLogger(LogLevel threshold = LogLevel::Info);

    /** Returns whether a message at the given level will be emitted. */
    bool should_log(LogLevel level) const override;

    /** Writes an entry through Drogon's matching native log level. */
    void log(LogLevel level, std::string_view message, const LogContext& context) override;

private:
    LogLevel threshold;
};

} // namespace stripekit