/** StripeApiClient.cpp
 *
 * See header file for details.
 */

#include "StripeKit/StripeApiClient.h"

#include "JsonValue.h"
#include "StripeKit/Callback.h"

#include <cctype>
#include <algorithm>
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

/** Require nonempty IDs before placing them in an encoded request path. */
std::string catalog_path(std::string_view group, const std::string& id) {
    if (id.empty()) throw ConfigurationException("A catalog object ID is required");
    return "/v1/" + std::string(group) + "/" + url_encode(id);
}

/** Encode product creation, retaining caller-supplied identities and metadata. */
std::string product_body(const CreateProductRequest& request) {
    if (request.name.empty()) throw ConfigurationException("A product name is required");
    std::string body;
    if (!request.id.empty()) append_form_value(body, "id", request.id);
    append_form_value(body, "name", request.name);
    append_form_value(body, "description", request.description);
    append_form_value(body, "active", request.active ? "true" : "false");
    append_metadata(body, request.metadata);
    return body;
}

/** Encode only supplied product changes, allowing explicit false and empty descriptions. */
std::string product_body(const UpdateProductRequest& request) {
    if (request.name && request.name->empty())
        throw ConfigurationException("An updated product name cannot be empty");
    std::string body;
    if (request.name) append_form_value(body, "name", *request.name);
    if (request.description) append_form_value(body, "description", *request.description);
    if (request.active) append_form_value(body, "active", *request.active ? "true" : "false");
    append_metadata(body, request.metadata);
    return body;
}

/** Validate and encode fixed per-unit pricing without changing existing subscriptions. */
std::string price_body(const CreatePriceRequest& request) {
    const auto& interval = request.recurring_interval;
    if (request.product_id.empty() || request.currency.size() != 3 ||
        !std::all_of(request.currency.begin(),
                     request.currency.end(),
                     [](unsigned char c) { return c >= 'a' && c <= 'z'; }) ||
        request.unit_amount < 0 || request.recurring_interval_count <= 0 ||
        (interval.empty() && request.recurring_interval_count != 1) ||
        (!interval.empty() && interval != "day" && interval != "week" && interval != "month" &&
         interval != "year") ||
        (!request.tax_behavior.empty() && request.tax_behavior != "inclusive" &&
         request.tax_behavior != "exclusive" && request.tax_behavior != "unspecified"))
        throw ConfigurationException("Invalid fixed-price creation request");
    std::string body;
    append_form_value(body, "product", request.product_id);
    append_form_value(body, "currency", request.currency);
    append_form_value(body, "unit_amount", std::to_string(request.unit_amount));
    if (!interval.empty()) {
        append_form_value(body, "recurring[interval]", interval);
        append_form_value(
            body, "recurring[interval_count]", std::to_string(request.recurring_interval_count));
        append_form_value(body, "recurring[usage_type]", "licensed");
    }
    if (!request.lookup_key.empty()) append_form_value(body, "lookup_key", request.lookup_key);
    if (!request.tax_behavior.empty())
        append_form_value(body, "tax_behavior", request.tax_behavior);
    append_metadata(body, request.metadata);
    return body;
}

/** Build a filtered price-list URL with explicit pagination. */
std::string prices_path(const ListPricesRequest& request) {
    if (request.limit < 1 || request.limit > 100 || request.lookup_keys.size() > 10)
        throw ConfigurationException("Invalid price-list limit or lookup-key count");
    std::string query;
    append_form_value(query, "limit", std::to_string(request.limit));
    if (!request.product_id.empty()) append_form_value(query, "product", request.product_id);
    if (request.active) append_form_value(query, "active", *request.active ? "true" : "false");
    if (!request.starting_after.empty())
        append_form_value(query, "starting_after", request.starting_after);
    for (const auto& key : request.lookup_keys) {
        if (key.empty()) throw ConfigurationException("Price lookup keys cannot be empty");
        append_form_value(query, "lookup_keys[]", key);
    }
    return "/v1/prices?" + query;
}

/** Require a typed field rather than disguising malformed responses as default values. */
void require_catalog_field(bool valid) {
    if (!valid) throw ConfigurationException("Stripe returned a malformed catalog object");
}

/** Read a complete price while preserving optional and unsupported pricing distinctions. */
Price price_value(const Json::Value& object) {
    require_catalog_field(
        object.isObject() && object["object"] == "price" && object["id"].isString() &&
        !object["id"].asString().empty() && object["active"].isBool() &&
        object["livemode"].isBool() && object["currency"].isString() && object["type"].isString() &&
        object["billing_scheme"].isString() &&
        (object["unit_amount"].isNull() || object["unit_amount"].isInt64()) &&
        (object["unit_amount_decimal"].isNull() || object["unit_amount_decimal"].isString()) &&
        (object["lookup_key"].isNull() || object["lookup_key"].isString()) &&
        (object["custom_unit_amount"].isNull() || object["custom_unit_amount"].isObject()));
    Price price;
    price.id = object["id"].asString();
    const auto& product = object["product"];
    price.product_id = product.isString() ? product.asString() : json_value::string(product, "id");
    require_catalog_field(!price.product_id.empty());
    price.currency = object["currency"].asString();
    price.type = object["type"].asString();
    price.billing_scheme = object["billing_scheme"].asString();
    price.tax_behavior = json_value::string(object, "tax_behavior");
    price.active = object["active"].asBool();
    price.livemode = object["livemode"].asBool();
    price.lookup_key = json_value::string(object, "lookup_key");
    if (!object["unit_amount"].isNull()) price.unit_amount = object["unit_amount"].asInt64();
    if (!object["unit_amount_decimal"].isNull())
        price.unit_amount_decimal = object["unit_amount_decimal"].asString();
    const auto& recurring = object["recurring"];
    require_catalog_field((price.type == "one_time" && recurring.isNull()) ||
                          (price.type == "recurring" && recurring.isObject()));
    if (!recurring.isNull()) {
        require_catalog_field(
            recurring["interval"].isString() && recurring["interval_count"].isInt64() &&
            recurring["interval_count"].asInt64() > 0 && recurring["usage_type"].isString());
        price.recurring = PriceRecurrence{ recurring["interval"].asString(),
                                           recurring["interval_count"].asInt64(),
                                           recurring["usage_type"].asString() };
    }
    const auto& transform = object["transform_quantity"];
    if (!transform.isNull()) {
        require_catalog_field(transform.isObject() && transform["divide_by"].isInt64() &&
                              transform["divide_by"].asInt64() > 0 &&
                              transform["round"].isString());
        price.transform_quantity = PriceQuantityTransform{ transform["divide_by"].asInt64(),
                                                           transform["round"].asString() };
    }
    price.custom_unit_amount = !object["custom_unit_amount"].isNull();
    price.metadata = metadata_value(object);
    return price;
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

Product StripeResponseParser::parse_product(const HttpResponse& response) const {
    return JsonStripeResponseParser{}.parse_product(response);
}

Price StripeResponseParser::parse_price(const HttpResponse& response) const {
    return JsonStripeResponseParser{}.parse_price(response);
}

PriceList StripeResponseParser::parse_price_list(const HttpResponse& response) const {
    return JsonStripeResponseParser{}.parse_price_list(response);
}

Product JsonStripeResponseParser::parse_product(const HttpResponse& response) const {
    const auto object = json_value::parse(response.body);
    require_catalog_field(object.isObject() && object["object"] == "product" &&
                          object["id"].isString() && !object["id"].asString().empty() &&
                          (object["deleted"].isNull() || object["deleted"].isBool()));
    Product product;
    product.id = object["id"].asString();
    product.deleted = object["deleted"] == true;
    if (product.deleted) return product;
    require_catalog_field(object["name"].isString() && object["active"].isBool() &&
                          object["livemode"].isBool() &&
                          (object["description"].isNull() || object["description"].isString()));
    product.name = object["name"].asString();
    product.description = json_value::string(object, "description");
    product.active = object["active"].asBool();
    product.livemode = object["livemode"].asBool();
    product.metadata = metadata_value(object);
    return product;
}

Price JsonStripeResponseParser::parse_price(const HttpResponse& response) const {
    return price_value(json_value::parse(response.body));
}

PriceList JsonStripeResponseParser::parse_price_list(const HttpResponse& response) const {
    const auto object = json_value::parse(response.body);
    require_catalog_field(object.isObject() && object["object"] == "list" &&
                          object["data"].isArray() && object["has_more"].isBool());
    PriceList result;
    result.has_more = object["has_more"].asBool();
    for (const auto& value : object["data"])
        result.data.push_back(price_value(value));
    return result;
}

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
      billing_portal_operations(*this), product_operations(*this), price_operations(*this) {}

StripeApiClient::StripeApiClient(HTTPClient& http_client,
                                 const StripeResponseParser& response_parser)
    : http_client(http_client), default_response_parser(), response_parser(response_parser),
      callback_lifetime(std::make_shared<CallbackLifetime>()), customer_operations(*this),
      checkout_session_operations(*this), checkout_operations(*this),
      subscription_operations(*this), billing_portal_session_operations(*this),
      billing_portal_operations(*this), product_operations(*this), price_operations(*this) {}

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

Products& StripeApiClient::products() {
    return product_operations;
}

Prices& StripeApiClient::prices() {
    return price_operations;
}

StripeApiClient::ProductOperations::ProductOperations(StripeApiClient& owner) : owner(owner) {}

Task<Product> StripeApiClient::ProductOperations::create_async(const CreateProductRequest& request,
                                                               const RequestOptions& options) {
    return send_and_parse_async<Product>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Post, "/v1/products", product_body(request), options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_product(response);
        });
}

void StripeApiClient::ProductOperations::create(const CreateProductRequest& request,
                                                const RequestOptions& options,
                                                Completion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Post, "/v1/products", product_body(request), options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Product>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_product(response);
        },
        std::move(completion));
}

Task<Product> StripeApiClient::ProductOperations::retrieve_async(std::string id,
                                                                 const RequestOptions& options) {
    return send_and_parse_async<Product>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, catalog_path("products", id), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_product(response);
        });
}

void StripeApiClient::ProductOperations::retrieve(std::string id,
                                                  const RequestOptions& options,
                                                  Completion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Get, catalog_path("products", id), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Product>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_product(response);
        },
        std::move(completion));
}

Task<Product> StripeApiClient::ProductOperations::update_async(std::string id,
                                                               const UpdateProductRequest& request,
                                                               const RequestOptions& options) {
    return send_and_parse_async<Product>(
        owner.http_client,
        owner.response_parser,
        make_request(
            HttpMethod::Post, catalog_path("products", id), product_body(request), options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_product(response);
        });
}

void StripeApiClient::ProductOperations::update(std::string id,
                                                const UpdateProductRequest& request,
                                                const RequestOptions& options,
                                                Completion completion) {
    HttpRequest http;
    try {
        http = make_request(
            HttpMethod::Post, catalog_path("products", id), product_body(request), options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Product>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_product(response);
        },
        std::move(completion));
}

StripeApiClient::PriceOperations::PriceOperations(StripeApiClient& owner) : owner(owner) {}

Task<Price> StripeApiClient::PriceOperations::create_async(const CreatePriceRequest& request,
                                                           const RequestOptions& options) {
    return send_and_parse_async<Price>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Post, "/v1/prices", price_body(request), options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_price(response);
        });
}

void StripeApiClient::PriceOperations::create(const CreatePriceRequest& request,
                                              const RequestOptions& options,
                                              Completion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Post, "/v1/prices", price_body(request), options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Price>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_price(response);
        },
        std::move(completion));
}

Task<Price> StripeApiClient::PriceOperations::retrieve_async(std::string id,
                                                             const RequestOptions& options) {
    return send_and_parse_async<Price>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, catalog_path("prices", id), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_price(response);
        });
}

void StripeApiClient::PriceOperations::retrieve(std::string id,
                                                const RequestOptions& options,
                                                Completion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Get, catalog_path("prices", id), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Price>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_price(response);
        },
        std::move(completion));
}

Task<PriceList> StripeApiClient::PriceOperations::list_async(const ListPricesRequest& request,
                                                             const RequestOptions& options) {
    return send_and_parse_async<PriceList>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, prices_path(request), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_price_list(response);
        });
}

void StripeApiClient::PriceOperations::list(const ListPricesRequest& request,
                                            const RequestOptions& options,
                                            ListCompletion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Get, prices_path(request), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<PriceList>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_price_list(response);
        },
        std::move(completion));
}

} // namespace stripekit