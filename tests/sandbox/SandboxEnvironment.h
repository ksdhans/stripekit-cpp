/** SandboxEnvironment.h
 *
 * Shared fixture for Stripe sandbox end-to-end tests. Hosts a local webhook
 * endpoint, forwards live Stripe events to it through the Stripe CLI, and
 * provides raw Stripe REST API access for objects the typed client does not
 * cover.
 *
 * @author Hans de Ruiter
 *
 * @License See LICENSE for details.
 */

#pragma once

#include "StripeKit/Adapters/Drogon.h"
#include "StripeKit/Client.h"
#include "StripeKit/HTTPClient.h"

#include <json/json.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace stripekit::testing {

/** Key/value pairs sent as an `application/x-www-form-urlencoded` request body. */
using FormParams = std::vector<std::pair<std::string, std::string>>;

/**
 * Settings that control whether and how the sandbox suite runs.
 *
 * All values come from environment variables so the same contract works for
 * local shells and CI runners. See tests/sandbox/README.md.
 */
struct SandboxConfig {
    /** True when `STRIPEKIT_SANDBOX_ENABLED` opts this machine in. */
    bool enabled = false;

    /** True when missing configuration should fail the run instead of skipping it. */
    bool required = false;

    /** Stripe sandbox secret key taken from `STRIPE_API_KEY`. */
    std::string api_key;

    /** Stripe CLI executable name or path. */
    std::string cli_path;

    /** How long to wait for an expected webhook event to arrive. */
    std::chrono::seconds event_timeout{90};

    /** Upper bound on Stripe API calls per run, guarding against runaway loops. */
    int max_api_calls = 400;

    /** Reads the configuration from the current process environment. */
    static SandboxConfig from_environment();

    /** Returns why the suite cannot run, or an empty string when it can. */
    std::string unavailable_reason() const;

    /** Returns true when the configuration is actively unsafe rather than merely absent. */
    bool is_unsafe() const;
};

/** Decoded response from a raw Stripe REST API call. */
struct StripeApiResult {
    /** HTTP status returned by Stripe. */
    int status_code = 0;

    /** Unparsed response body. */
    std::string raw_body;

    /** Response body parsed as JSON. */
    Json::Value body;

    /** Returns true when Stripe returned a 2xx status. */
    bool ok() const;

    /** Returns the `id` field of the returned object, or an empty string. */
    std::string id() const;

    /** Returns a single-line summary suitable for a test failure message. */
    std::string describe() const;
};

/**
 * Sends form-encoded requests to Stripe's REST API.
 *
 * The typed StripeKit client covers the supported product surface. These tests
 * also need to create prices, subscriptions, payments, and disputes in order to
 * make Stripe emit the events under test, so those calls go through here.
 */
class SandboxStripeApi {
public:
    /** Creates an API helper bound to a sandbox secret key. */
    SandboxStripeApi(std::string api_key, int max_calls);

    /** Destroys the helper. */
    ~SandboxStripeApi();

    /** Sends a form-encoded POST and returns the decoded response. */
    StripeApiResult post(std::string_view path, const FormParams& params);

    /** Sends a GET and returns the decoded response. */
    StripeApiResult get(std::string_view path);

    /** Sends a POST and throws when Stripe does not return a success status. */
    StripeApiResult require_post(std::string_view path, const FormParams& params);

    /** Returns how many Stripe API calls this run has made. */
    int call_count() const;

private:
    StripeApiResult send(HttpMethod method, std::string_view path, const FormParams& params);

    stripekit::drogon::HTTPClient http_client;
    int max_calls;
    std::atomic<int> calls{0};
};

/** A raw webhook delivery captured exactly as Stripe signed and sent it. */
struct CapturedDelivery {
    /** Stripe event id parsed from the payload. */
    std::string event_id;

    /** Raw request body bytes. */
    std::string payload;

    /** Value of the `Stripe-Signature` header. */
    std::string signature_header;
};

/** A verified Stripe event that reached the local webhook handler. */
struct ReceivedEvent {
    /** Stripe event id. */
    std::string id;

    /** Stripe event type, for example `invoice.paid`. */
    std::string type;

    /** Raw event JSON payload. */
    std::string payload_json;

    /** Returns the event payload parsed as JSON. */
    Json::Value json() const;
};

/**
 * Runs the local webhook server and Stripe CLI listener for the sandbox suite.
 *
 * Call start once per test executable. When it returns false the suite is not
 * configured to run and unavailable_reason explains why.
 */
class SandboxEnvironment {
public:
    /** Returns the process-wide environment instance. */
    static SandboxEnvironment& get();

    /** Route that the Stripe CLI forwards live events to. */
    static constexpr const char* main_webhook_path = "/stripe/webhook";

    /** Route whose handler reports a retryable failure on the first attempt per event. */
    static constexpr const char* retry_webhook_path = "/stripe/webhook/retry-once";

    /** Starts the webhook server and event listener, returning false when unavailable. */
    bool start();

    /** Stops the listener and webhook server. */
    void stop();

    /** Returns the resolved run configuration. */
    const SandboxConfig& config() const;

    /** Returns why the suite could not start, or an empty string. */
    const std::string& unavailable_reason() const;

    /** Returns the raw Stripe REST API helper. */
    SandboxStripeApi& stripe_api();

    /** Returns a StripeKit client pointed at the live Stripe sandbox. */
    StripeClient& stripe_client();

    /** Returns the identifier stamped onto every object this run creates. */
    const std::string& run_id() const;

    /**
     * Waits for a delivered event of the given type whose payload mentions an id.
     *
     * Matching on an id created by the calling test keeps concurrent runs that
     * share a sandbox from consuming each other's events.
     */
    std::optional<ReceivedEvent> wait_for_event(std::string_view type, std::string_view must_contain);

    /** Returns the raw delivery captured for an event id, when one arrived. */
    std::optional<CapturedDelivery> delivery_for(std::string_view event_id) const;

    /** Returns how many times the main handler body ran for an event id. */
    int dispatch_count(std::string_view event_id) const;

    /** Returns how many times the retry route's handler ran for an event id. */
    int retry_route_attempts(std::string_view event_id) const;

    /** Posts a payload and signature to a local webhook route and returns the HTTP status. */
    int post_webhook(std::string_view path,
                     std::string_view payload,
                     std::string_view signature_header) const;

private:
    SandboxEnvironment();
    ~SandboxEnvironment();

    SandboxEnvironment(const SandboxEnvironment&) = delete;
    SandboxEnvironment& operator=(const SandboxEnvironment&) = delete;

    struct State;
    std::unique_ptr<State> state;
};

/** Returns a nested value addressed by a dotted path, or null when absent. */
Json::Value json_path(const Json::Value& root, std::string_view dotted_path);

/** Returns an email address unique to this run, for creating sandbox customers. */
std::string sandbox_email(const SandboxEnvironment& environment, std::string_view label);

/**
 * Creates a sandbox customer with a test card attached as the default payment method.
 *
 * Pass a Stripe test payment method such as `pm_card_visa` for a card that
 * succeeds, or `pm_card_chargeDeclined` for one that fails.
 */
std::string create_customer_with_card(SandboxEnvironment& environment,
                                      std::string_view test_payment_method,
                                      std::string_view label);

/** Creates a product with a recurring monthly price and returns the price id. */
std::string create_monthly_price(SandboxEnvironment& environment, std::int64_t amount_cents);

/** Creates and confirms a one-off payment, returning the resulting charge id. */
std::string create_confirmed_charge(SandboxEnvironment& environment,
                                    std::string_view test_payment_method,
                                    std::int64_t amount_cents);

} // namespace stripekit::testing
