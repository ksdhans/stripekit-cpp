/** StripeApiClient.cpp
 *
 * See header file for details.
 */

#include "StripeKit/StripeApiClient.h"

#include "JsonValue.h"
#include "StripeKit/Callback.h"

#include <cctype>
#include <condition_variable>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>

namespace stripekit {

/** Coordinates client shutdown with callback-based response parsing. */
struct CallbackLifetime {
    std::mutex mutex;
    std::condition_variable finished;
    std::size_t active = 0;
    bool closing = false;

    /** Reserves a callback operation unless shutdown has started. */
    bool acquire() {
        std::lock_guard<std::mutex> lock(mutex);
        if (closing) {
            return false;
        }
        ++active;
        return true;
    }

    /** Releases one callback operation and wakes a waiting destructor. */
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        --active;
        if (active == 0) {
            finished.notify_all();
        }
    }

    /** Prevents new operations and waits for all active callbacks. */
    void close_and_wait() {
        std::unique_lock<std::mutex> lock(mutex);
        closing = true;
        finished.wait(lock, [this]() { return active == 0; });
    }
};

namespace {

/** Releases a callback lifetime reservation exactly once. */
class CallbackLifetimeLease final {
public:
    explicit CallbackLifetimeLease(std::shared_ptr<CallbackLifetime> lifetime)
        : lifetime(std::move(lifetime)) {}

    ~CallbackLifetimeLease() {
        release();
    }

    /** Releases the reservation before user code is called. */
    void release() {
        if (active) {
            active = false;
            lifetime->release();
        }
    }

private:
    std::shared_ptr<CallbackLifetime> lifetime;
    bool active = true;
};

std::unordered_map<std::string, std::string> metadata_value(const Json::Value& object) {
    std::unordered_map<std::string, std::string> metadata;
    const auto& value = object["metadata"];
    if (!value.isObject()) {
        return metadata;
    }
    for (const auto& name : value.getMemberNames()) {
        if (value[name].isString()) {
            metadata.emplace(name, value[name].asString());
        }
    }
    return metadata;
}

StripeErrorType error_type(std::string_view value) {
    if (value == "api_error") return StripeErrorType::Api;
    if (value == "card_error") return StripeErrorType::Card;
    if (value == "idempotency_error") return StripeErrorType::Idempotency;
    if (value == "invalid_request_error") return StripeErrorType::InvalidRequest;
    if (value == "authentication_error") return StripeErrorType::Authentication;
    if (value == "permission_error") return StripeErrorType::Permission;
    if (value == "rate_limit_error") return StripeErrorType::RateLimit;
    if (value == "api_connection_error") return StripeErrorType::ApiConnection;
    return StripeErrorType::Unknown;
}

bool retryable_status(int status) {
    return status == 409 || status == 429 || status >= 500;
}

std::string url_encode(std::string_view value) {
    std::ostringstream output;
    output.fill('0');
    output << std::hex << std::uppercase;

    for (const unsigned char character : value) {
        if (std::isalnum(character) || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            output << character;
        } else {
            output << '%' << std::setw(2) << static_cast<int>(character);
        }
    }
    return output.str();
}

void append_form_value(std::string& body, std::string_view name, std::string_view value) {
    if (!body.empty()) {
        body += '&';
    }
    body += url_encode(name);
    body += '=';
    body += url_encode(value);
}

void append_metadata(std::string& body,
                     const std::unordered_map<std::string, std::string>& metadata) {
    for (const auto& [key, value] : metadata) {
        append_form_value(body, "metadata[" + key + "]", value);
    }
}

HttpRequest
make_request(HttpMethod method, std::string path, std::string body, const RequestOptions& options) {
    HttpRequest request{
        .method = method, .path = std::move(path), .body = std::move(body), .options = options
    };

    request.headers.emplace("Accept", "application/json");
    if (method == HttpMethod::Post) {
        request.headers.emplace("Content-Type", "application/x-www-form-urlencoded");
        if (options.idempotency_key.has_value()) {
            request.headers.emplace("Idempotency-Key", *options.idempotency_key);
        }
    }
    for (const auto& [name, value] : options.headers) {
        request.headers.insert_or_assign(name, value);
    }
    return request;
}

bool is_success(const HttpResponse& response) {
    return response.status_code >= 200 && response.status_code < 300;
}

std::string checkout_session_body(const CreateCheckoutSessionRequest& request) {
    std::string body;
    const std::string session_mode = request.mode.empty() ? "subscription" : request.mode;
    append_form_value(body, "mode", session_mode);
    if (!request.customer_id.empty()) {
        append_form_value(body, "customer", request.customer_id);
    }
    if (!request.price_id.empty()) {
        append_form_value(body, "line_items[0][price]", request.price_id);
    } else {
        append_form_value(body, "line_items[0][price_data][currency]", request.currency);
        append_form_value(
            body, "line_items[0][price_data][unit_amount]", std::to_string(request.unit_amount));
        if (session_mode == "subscription" && !request.recurring_interval.empty()) {
            append_form_value(
                body, "line_items[0][price_data][recurring][interval]", request.recurring_interval);
        }
        append_form_value(
            body, "line_items[0][price_data][product_data][name]", request.product_name);
    }
    append_form_value(body, "line_items[0][quantity]", std::to_string(request.quantity));
    append_form_value(body, "success_url", request.success_url);
    append_form_value(body, "cancel_url", request.cancel_url);
    append_metadata(body, request.metadata);
    return body;
}

std::string billing_portal_session_body(const CreateBillingPortalSessionRequest& request) {
    std::string body;
    append_form_value(body, "customer", request.customer_id);
    append_form_value(body, "return_url", request.return_url);
    return body;
}

template <typename TResult, typename TParser>
Task<TResult> send_and_parse_async(HTTPClient& http_client,
                                   const StripeResponseParser& response_parser,
                                   HttpRequest request,
                                   TParser parser) {
    const auto response = co_await http_client.send_async(request);
    if (!is_success(response)) {
        throw StripeApiException(response_parser.parse_error(response));
    }
    co_return parser(response_parser, response);
}

template <typename TResult, typename TParser, typename TCompletion>
void send_and_parse(HTTPClient& http_client,
                    const StripeResponseParser& response_parser,
                    std::shared_ptr<CallbackLifetime> callback_lifetime,
                    HttpRequest request,
                    TParser parser,
                    TCompletion completion) {
    auto once_completion =
        std::make_shared<OnceCallback<CallbackResult<TResult>>>(std::move(completion));
    if (!callback_lifetime->acquire()) {
        once_completion->invoke(
            std::make_exception_ptr(StripeKitException("Stripe API client is shutting down")));
        return;
    }

    try {
        http_client.send(
            request,
            [callback_lifetime, once_completion, &response_parser, parser = std::move(parser)](
                std::variant<HttpResponse, std::exception_ptr> result) mutable {
                CallbackLifetimeLease lease(callback_lifetime);
                try {
                    if (const auto* error = std::get_if<std::exception_ptr>(&result)) {
                        lease.release();
                        once_completion->invoke(*error);
                        return;
                    }
                    const auto& response = std::get<HttpResponse>(result);
                    if (!is_success(response)) {
                        auto error = std::make_exception_ptr(
                            StripeApiException(response_parser.parse_error(response)));
                        lease.release();
                        once_completion->invoke(error);
                        return;
                    }
                    auto parsed = parser(response_parser, response);
                    lease.release();
                    once_completion->invoke(std::move(parsed));
                } catch (...) {
                    lease.release();
                    once_completion->invoke(std::current_exception());
                }
            });
    } catch (...) {
        callback_lifetime->release();
        once_completion->invoke(std::current_exception());
    }
}

} // namespace

Customer JsonStripeResponseParser::parse_customer(const HttpResponse& response) const {
    const auto object = json_value::parse(response.body);
    return Customer{ .id = json_value::string(object, "id"),
                     .email = json_value::string(object, "email"),
                     .name = json_value::string(object, "name"),
                     .livemode = json_value::boolean(object, "livemode"),
                     .metadata = metadata_value(object) };
}

CheckoutSession
JsonStripeResponseParser::parse_checkout_session(const HttpResponse& response) const {
    const auto object = json_value::parse(response.body);
    const auto string_or_object_id = [](const Json::Value& obj, const char* name) -> std::string {
        if (!obj.isObject() || !obj.isMember(name)) {
            return {};
        }
        const auto& val = obj[name];
        if (val.isString()) {
            return val.asString();
        }
        if (val.isObject() && val.isMember("id") && val["id"].isString()) {
            return val["id"].asString();
        }
        return {};
    };

    return CheckoutSession{ .id = json_value::string(object, "id"),
                            .customer_id = string_or_object_id(object, "customer"),
                            .subscription_id = string_or_object_id(object, "subscription"),
                            .payment_intent_id = string_or_object_id(object, "payment_intent"),
                            .status = json_value::string(object, "status"),
                            .mode = json_value::string(object, "mode"),
                            .url = json_value::string(object, "url"),
                            .currency = json_value::string(object, "currency"),
                            .amount_total = json_value::integer(object, "amount_total"),
                            .livemode = json_value::boolean(object, "livemode"),
                            .metadata = metadata_value(object) };
}

Subscription JsonStripeResponseParser::parse_subscription(const HttpResponse& response) const {
    const auto object = json_value::parse(response.body);
    return Subscription{
        .id = json_value::string(object, "id"),
        .customer_id = json_value::string(object, "customer"),
        .status = json_value::string(object, "status"),
        .current_period_start_unix = json_value::integer(object, "current_period_start"),
        .current_period_end_unix = json_value::integer(object, "current_period_end"),
        .cancel_at_period_end = json_value::boolean(object, "cancel_at_period_end"),
        .livemode = json_value::boolean(object, "livemode"),
        .metadata = metadata_value(object)
    };
}

BillingPortalSession
JsonStripeResponseParser::parse_billing_portal_session(const HttpResponse& response) const {
    const auto object = json_value::parse(response.body);
    return BillingPortalSession{ .id = json_value::string(object, "id"),
                                 .customer_id = json_value::string(object, "customer"),
                                 .url = json_value::string(object, "url"),
                                 .return_url = json_value::string(object, "return_url"),
                                 .created_unix = json_value::integer(object, "created"),
                                 .livemode = json_value::boolean(object, "livemode") };
}

StripeErrorPayload JsonStripeResponseParser::parse_error(const HttpResponse& response) const {
    StripeErrorPayload payload{ .type = StripeErrorType::Unknown,
                                .message = "Stripe API request failed",
                                .http_status = response.status_code,
                                .retryable = retryable_status(response.status_code) };
    const auto request_id = response.headers.find("request-id");
    if (request_id != response.headers.end()) {
        payload.request_id = request_id->second;
    }
    Json::Value root;
    try {
        root = json_value::parse(response.body);
    } catch (const ConfigurationException&) {
        payload.message = "Stripe API request failed with an invalid JSON error response";
        return payload;
    }
    if (root.isObject() && root["error"].isObject()) {
        const auto& error = root["error"];
        payload.type = error_type(json_value::string(error, "type"));
        payload.code = json_value::string(error, "code");
        const auto message = json_value::string(error, "message");
        if (!message.empty()) {
            payload.message = message;
        }
    }
    return payload;
}

StripeApiClient::CustomerOperations::CustomerOperations(StripeApiClient& owner) : owner(owner) {}

Task<Customer>
StripeApiClient::CustomerOperations::create_async(const CreateCustomerRequest& request,
                                                  const RequestOptions& options) {
    std::string body;
    append_form_value(body, "email", request.email);
    append_form_value(body, "name", request.name);
    append_metadata(body, request.metadata);
    return send_and_parse_async<Customer>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Post, "/v1/customers", std::move(body), options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_customer(response);
        });
}

void StripeApiClient::CustomerOperations::create(const CreateCustomerRequest& request,
                                                 const RequestOptions& options,
                                                 CreateCompletion completion) {
    std::string body;
    append_form_value(body, "email", request.email);
    append_form_value(body, "name", request.name);
    append_metadata(body, request.metadata);
    send_and_parse<Customer>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        make_request(HttpMethod::Post, "/v1/customers", std::move(body), options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_customer(response);
        },
        std::move(completion));
}

Task<Customer> StripeApiClient::CustomerOperations::retrieve_async(std::string customer_id,
                                                                   const RequestOptions& options) {
    return send_and_parse_async<Customer>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, "/v1/customers/" + url_encode(customer_id), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_customer(response);
        });
}

void StripeApiClient::CustomerOperations::retrieve(std::string customer_id,
                                                   const RequestOptions& options,
                                                   RetrieveCompletion completion) {
    send_and_parse<Customer>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        make_request(HttpMethod::Get, "/v1/customers/" + url_encode(customer_id), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_customer(response);
        },
        std::move(completion));
}

StripeApiClient::CheckoutSessionOperations::CheckoutSessionOperations(StripeApiClient& owner)
    : owner(owner) {}

Task<CheckoutSession> StripeApiClient::CheckoutSessionOperations::create_async(
    const CreateCheckoutSessionRequest& request, const RequestOptions& options) {
    return send_and_parse_async<CheckoutSession>(
        owner.http_client,
        owner.response_parser,
        make_request(
            HttpMethod::Post, "/v1/checkout/sessions", checkout_session_body(request), options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_checkout_session(response);
        });
}

void StripeApiClient::CheckoutSessionOperations::create(const CreateCheckoutSessionRequest& request,
                                                        const RequestOptions& options,
                                                        CreateCompletion completion) {
    send_and_parse<CheckoutSession>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        make_request(
            HttpMethod::Post, "/v1/checkout/sessions", checkout_session_body(request), options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_checkout_session(response);
        },
        std::move(completion));
}

StripeApiClient::CheckoutOperations::CheckoutOperations(StripeApiClient& owner) : owner(owner) {}

CheckoutSessions& StripeApiClient::CheckoutOperations::sessions() {
    return owner.checkout_session_operations;
}

StripeApiClient::SubscriptionOperations::SubscriptionOperations(StripeApiClient& owner)
    : owner(owner) {}

Task<Subscription>
StripeApiClient::SubscriptionOperations::retrieve_async(std::string subscription_id,
                                                        const RequestOptions& options) {
    return send_and_parse_async<Subscription>(
        owner.http_client,
        owner.response_parser,
        make_request(
            HttpMethod::Get, "/v1/subscriptions/" + url_encode(subscription_id), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription(response);
        });
}

void StripeApiClient::SubscriptionOperations::retrieve(std::string subscription_id,
                                                       const RequestOptions& options,
                                                       RetrieveCompletion completion) {
    send_and_parse<Subscription>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        make_request(
            HttpMethod::Get, "/v1/subscriptions/" + url_encode(subscription_id), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription(response);
        },
        std::move(completion));
}

Task<Subscription>
StripeApiClient::SubscriptionOperations::cancel_async(std::string subscription_id,
                                                      const RequestOptions& options) {
    return send_and_parse_async<Subscription>(
        owner.http_client,
        owner.response_parser,
        make_request(
            HttpMethod::Delete, "/v1/subscriptions/" + url_encode(subscription_id), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription(response);
        });
}

void StripeApiClient::SubscriptionOperations::cancel(std::string subscription_id,
                                                     const RequestOptions& options,
                                                     CancelCompletion completion) {
    send_and_parse<Subscription>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        make_request(
            HttpMethod::Delete, "/v1/subscriptions/" + url_encode(subscription_id), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription(response);
        },
        std::move(completion));
}

StripeApiClient::BillingPortalSessionOperations::BillingPortalSessionOperations(
    StripeApiClient& owner)
    : owner(owner) {}

Task<BillingPortalSession> StripeApiClient::BillingPortalSessionOperations::create_async(
    const CreateBillingPortalSessionRequest& request, const RequestOptions& options) {
    return send_and_parse_async<BillingPortalSession>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Post,
                     "/v1/billing_portal/sessions",
                     billing_portal_session_body(request),
                     options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_billing_portal_session(response);
        });
}

void StripeApiClient::BillingPortalSessionOperations::create(
    const CreateBillingPortalSessionRequest& request,
    const RequestOptions& options,
    CreateCompletion completion) {
    send_and_parse<BillingPortalSession>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        make_request(HttpMethod::Post,
                     "/v1/billing_portal/sessions",
                     billing_portal_session_body(request),
                     options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_billing_portal_session(response);
        },
        std::move(completion));
}

StripeApiClient::BillingPortalOperations::BillingPortalOperations(StripeApiClient& owner)
    : owner(owner) {}

BillingPortalSessions& StripeApiClient::BillingPortalOperations::sessions() {
    return owner.billing_portal_session_operations;
}

StripeApiClient::StripeApiClient(HTTPClient& http_client)
    : http_client(http_client), default_response_parser(), response_parser(default_response_parser),
      callback_lifetime(std::make_shared<CallbackLifetime>()), customer_operations(*this),
      checkout_session_operations(*this), checkout_operations(*this),
      subscription_operations(*this), billing_portal_session_operations(*this),
      billing_portal_operations(*this) {}

StripeApiClient::StripeApiClient(HTTPClient& http_client,
                                 const StripeResponseParser& response_parser)
    : http_client(http_client), default_response_parser(), response_parser(response_parser),
      callback_lifetime(std::make_shared<CallbackLifetime>()), customer_operations(*this),
      checkout_session_operations(*this), checkout_operations(*this),
      subscription_operations(*this), billing_portal_session_operations(*this),
      billing_portal_operations(*this) {}

StripeApiClient::~StripeApiClient() {
    callback_lifetime->close_and_wait();
}

Customers& StripeApiClient::customers() {
    return customer_operations;
}

Checkout& StripeApiClient::checkout() {
    return checkout_operations;
}

Subscriptions& StripeApiClient::subscriptions() {
    return subscription_operations;
}

BillingPortal& StripeApiClient::billing_portal() {
    return billing_portal_operations;
}

} // namespace stripekit