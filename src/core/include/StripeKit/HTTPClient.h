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
 * @addtogroup adapter_interfaces
 * @{
 */

/**
 * HTTP method for a Stripe API request.
 */
enum class HttpMethod {
    Get,   /**< Fetch an object. */
    Post,  /**< Create or update an object. */
    Delete /**< Remove an object. */
};

/**
 * A request StripeKit wants sent to Stripe.
 *
 * StripeKit fills this in; the adapter turns it into a real HTTP request.
 */
struct HttpRequest {
    /** Method to use. */
    HttpMethod method = HttpMethod::Get;

    /**
     * Path relative to the Stripe API base URL, for example "/v1/customers".
     *
     * Join it to the base URL, normally "https://api.stripe.com".
     */
    std::string path;

    /**
     * Headers StripeKit needs sent.
     *
     * The adapter adds its own on top, notably the "Authorization" header
     * carrying the secret key.
     */
    std::unordered_map<std::string, std::string> headers;

    /**
     * The request body, already encoded as "application/x-www-form-urlencoded".
     *
     * Send it unchanged. Stripe's API does not accept JSON request bodies.
     */
    std::string body;

    /**
     * Per-call settings the adapter must honour.
     *
     * `idempotency_key` becomes the "Idempotency-Key" header when set,
     * `timeout` overrides the adapter's default, and `headers` are merged over
     * the adapter's defaults. Ignoring the idempotency key means a retried call
     * can charge a customer twice.
     */
    RequestOptions options;
};

/**
 * Stripe's answer to a request.
 */
struct HttpResponse {
    /** HTTP status Stripe returned. */
    int status_code = 0;

    /** Response headers, with lower-case keys. StripeKit looks them up that way. */
    std::unordered_map<std::string, std::string> headers;

    /** Response body, unmodified. */
    std::string body;
};

/**
 * Sends StripeKit's requests to Stripe.
 *
 * This is the outbound half of an adapter. Implement it to put StripeKit on
 * top of an existing HTTP stack; applications normally use a ready-made
 * adapter such as stripekit::drogon::HTTPClient instead.
 *
 * A rejected request is not an error here. Report a 4xx or 5xx from Stripe as
 * a normal HttpResponse and let StripeKit parse Stripe's error document and
 * throw StripeApiException itself. Reserve exceptions for failures that stopped
 * the request reaching Stripe at all, such as DNS, TLS, or timeout failures.
 *
 * Implementations must be thread-safe and must tolerate several requests being
 * in flight at once.
 *
 * @sa @ref writing_an_adapter
 */
class HTTPClient {
public:
    /** Completion callback for the callback form of send(). */
    using Completion = std::function<void(std::variant<HttpResponse, std::exception_ptr>)>;

    /**
     * Destroys the HTTP client instance.
     */
    virtual ~HTTPClient() = default;

    /**
     * Sends a request to Stripe.
     *
     * @param request What to send, including the options to honour.
     * @return A task producing Stripe's response.
     * @throws std::exception Stripe could not be reached. A response with an
     *         error status is returned normally, not thrown.
     */
    virtual Task<HttpResponse> send_async(const HttpRequest& request) = 0;

    /**
     * Sends a request to Stripe, reporting the result through a callback.
     *
     * The callback must be invoked exactly once, on every path including the
     * failure paths. Calling it twice corrupts whatever is waiting on it;
     * never calling it hangs the request. OnceCallback exists to enforce this.
     *
     * @param request What to send, including the options to honour.
     * @param completion Receives Stripe's response, or the transport failure.
     */
    virtual void send(const HttpRequest& request, Completion completion) = 0;
};

/** @} */

} // namespace stripekit