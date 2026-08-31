/** HTTPClient.h
 *
 * Backend API used to communicate with Stripe's REST API.
 *
 * @author Hans de Ruiter
 *
 * @License See LICENSE for details.
 */

#pragma once

#include "StripeKit/Async.h"
#include "StripeKit/RequestOptions.h"

#include <exception>
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>

namespace stripekit {

/**
 * HTTP methods used when calling Stripe's API.
 */
enum class HttpMethod {
    Get,
    Post,
    Delete
};

/**
 * Represents an outbound HTTP request issued to Stripe.
 */
struct HttpRequest {
    /** HTTP method used by the request. */
    HttpMethod method = HttpMethod::Get;

    /** Request path relative to the Stripe API base URL. */
    std::string path;

    /** Outbound request headers. */
    std::unordered_map<std::string, std::string> headers;

    /** Outbound request body payload. */
    std::string body;

    /** Shared request options including idempotency, timeout, and header overrides. */
    RequestOptions options;
};

/**
 * Represents a normalized HTTP response returned from Stripe.
 */
struct HttpResponse {
    /** HTTP status code. */
    int status_code = 0;

    /** Response headers. */
    std::unordered_map<std::string, std::string> headers;

    /** Response payload body. */
    std::string body;
};

/**
 * Abstract base class for making HTTP calls to Stripe.
 */
class HTTPClient {
public:
    /** Callback type used by compatibility overloads. */
    using Completion = std::function<void(std::variant<HttpResponse, std::exception_ptr>)>;

    /**
     * Destroys the HTTP client instance.
     */
    virtual ~HTTPClient() = default;

    /**
     * Starts a coroutine that sends an HTTP request to Stripe.
     */
    virtual Task<HttpResponse> send_async(const HttpRequest& request) = 0;

    /**
     * Sends a request using callback compatibility semantics.
     *
     * Implementations must complete exactly once.
     */
    virtual void send(const HttpRequest& request, Completion completion) = 0;
};

} // namespace stripekit