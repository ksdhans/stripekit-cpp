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
#include <initializer_list>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>
#include <unordered_set>

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

/** Recognize the Stripe API version header regardless of HTTP header casing. */
bool is_stripe_version_header(std::string_view name) {
    constexpr std::string_view expected = "stripe-version";
    return std::equal(
        name.begin(),
        name.end(),
        expected.begin(),
        expected.end(),
        [](unsigned char left, unsigned char right) { return std::tolower(left) == right; });
}

/** Return the version used to encode and send a request. */
std::string_view stripe_api_version(const RequestOptions& options) {
    for (const auto& [name, value] : options.headers) {
        if (is_stripe_version_header(name)) return value;
    }
    return default_stripe_api_version;
}

/** Build a versioned API request, allowing callers to override its default headers. */
HttpRequest
make_request(HttpMethod method, std::string path, std::string body, const RequestOptions& options) {
    HttpRequest request{
        .method = method, .path = std::move(path), .body = std::move(body), .options = options
    };

    request.headers.emplace("Accept", "application/json");
    request.headers.emplace("Stripe-Version", stripe_api_version(options));
    if (method == HttpMethod::Post) {
        request.headers.emplace("Content-Type", "application/x-www-form-urlencoded");
        if (options.idempotency_key.has_value()) {
            request.headers.emplace("Idempotency-Key", *options.idempotency_key);
        }
    }
    for (const auto& [name, value] : options.headers) {
        if (is_stripe_version_header(name)) continue;
        request.headers.insert_or_assign(name, value);
    }
    return request;
}

/** Require nonempty IDs before placing them in an encoded request path. */
std::string catalog_path(std::string_view group, const std::string& id) {
    if (id.empty()) throw ConfigurationException("A catalog object ID is required");
    return "/v1/" + std::string(group) + "/" + url_encode(id);
}

/** Build a Checkout list query with bounded pagination and encoded filter values. */
std::string checkout_sessions_path(const ListCheckoutSessionsRequest& request) {
    if (request.limit < 1 || request.limit > 100)
        throw ConfigurationException("Checkout session page size must be from 1 to 100");
    std::string query;
    append_form_value(query, "limit", std::to_string(request.limit));
    if (!request.payment_intent_id.empty())
        append_form_value(query, "payment_intent", request.payment_intent_id);
    if (!request.starting_after.empty())
        append_form_value(query, "starting_after", request.starting_after);
    return "/v1/checkout/sessions?" + query;
}

/** Build the validated query for one Checkout session's purchased line items. */
std::string checkout_line_items_path(const std::string& session_id,
                                     const ListPurchasedLineItemsRequest& request) {
    if (session_id.empty())
        throw ConfigurationException("Checkout line items require a session ID");
    if (request.limit < 1 || request.limit > 100)
        throw ConfigurationException("Checkout line-item page size must be from 1 to 100");
    std::string query;
    append_form_value(query, "limit", std::to_string(request.limit));
    if (!request.starting_after.empty())
        append_form_value(query, "starting_after", request.starting_after);
    return "/v1/checkout/sessions/" + url_encode(session_id) + "/line_items?" + query;
}

/** Build the validated query for one invoice's purchased line items. */
std::string invoice_line_items_path(const std::string& invoice_id,
                                    const ListPurchasedLineItemsRequest& request) {
    if (invoice_id.empty())
        throw ConfigurationException("Invoice line items require an invoice ID");
    if (request.limit < 1 || request.limit > 100)
        throw ConfigurationException("Invoice line-item page size must be from 1 to 100");
    std::string query;
    append_form_value(query, "limit", std::to_string(request.limit));
    if (!request.starting_after.empty())
        append_form_value(query, "starting_after", request.starting_after);
    return "/v1/invoices/" + url_encode(invoice_id) + "/lines?" + query;
}

/** Encode product creation, retaining caller-supplied identities and metadata. */
std::string product_body(const CreateProductRequest& request) {
    if (request.name.empty()) throw ConfigurationException("A product name is required");
    std::string body;
    if (!request.id.empty()) append_form_value(body, "id", request.id);
    append_form_value(body, "name", request.name);
    if (!request.description.empty()) append_form_value(body, "description", request.description);
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
    if (request.default_price_id) {
        if (request.default_price_id->empty())
            throw ConfigurationException("A default price ID cannot be empty");
        append_form_value(body, "default_price", *request.default_price_id);
    }
    append_metadata(body, request.metadata);
    return body;
}

/** Update mutable details without changing the price's amount or recurrence. */
std::string price_body(const UpdatePriceRequest& request) {
    if (request.transfer_lookup_key.value_or(false) &&
        (!request.lookup_key || request.lookup_key->empty()))
        throw ConfigurationException("Transferring a lookup key requires a nonempty lookup key");
    std::string body;
    if (request.active) append_form_value(body, "active", *request.active ? "true" : "false");
    if (request.nickname) append_form_value(body, "nickname", *request.nickname);
    if (request.lookup_key) append_form_value(body, "lookup_key", *request.lookup_key);
    if (request.transfer_lookup_key)
        append_form_value(
            body, "transfer_lookup_key", *request.transfer_lookup_key ? "true" : "false");
    append_metadata(body, request.metadata);
    return body;
}

/** Encode an explicit policy; invalid enum values must not become a Stripe default. */
std::string proration_value(ProrationBehavior behavior) {
    switch (behavior) {
    case ProrationBehavior::CreateProrations:
        return "create_prorations";
    case ProrationBehavior::AlwaysInvoice:
        return "always_invoice";
    case ProrationBehavior::None:
        return "none";
    }
    throw ConfigurationException("Invalid proration behavior");
}

/** Build the request body for creating a schedule from an existing subscription.
 *
 * @param request Identifies the existing subscription.
 *
 * @return Form data to send to Stripe.
 *
 * @throws ConfigurationException If the subscription ID is empty.
 */
std::string schedule_create_body(const CreateSubscriptionScheduleRequest& request) {
    if (request.from_subscription.empty())
        throw ConfigurationException("Schedule creation requires a subscription ID");
    std::string body;
    append_form_value(body, "from_subscription", request.from_subscription);
    return body;
}

/** Append a schedule discount while requiring exactly one reusable reference. */
void append_schedule_discount(std::string& body,
                              const std::string& prefix,
                              const SubscriptionScheduleDiscount& discount) {
    if ((discount.coupon_id.empty() && discount.promotion_code_id.empty() &&
         discount.discount_id.empty()) ||
        (!discount.coupon_id.empty() && !discount.promotion_code_id.empty()))
        throw ConfigurationException(
            "A schedule discount requires one reusable discount, coupon, or promotion-code ID");
    if (!discount.promotion_code_id.empty())
        append_form_value(body, prefix + "[promotion_code]", discount.promotion_code_id);
    else if (!discount.coupon_id.empty())
        append_form_value(body, prefix + "[coupon]", discount.coupon_id);
    else append_form_value(body, prefix + "[discount]", discount.discount_id);
}

/** Build the request body for replacing a schedule's current and future phases.
 *
 * @param request Replacement phases, end behavior, and payment adjustment policies.
 *
 * @return Form data to send to Stripe.
 *
 * @throws ConfigurationException If a phase has invalid dates, items, or settings.
 */
std::string schedule_update_body(const UpdateSubscriptionScheduleRequest& request) {
    if (request.phases.empty() || request.phases.size() > 10)
        throw ConfigurationException("A schedule update requires one through ten phases");
    std::string end;
    switch (request.end_behavior) {
    case SubscriptionScheduleEndBehavior::Release:
        end = "release";
        break;
    case SubscriptionScheduleEndBehavior::Cancel:
        end = "cancel";
        break;
    default:
        throw ConfigurationException("Invalid schedule end behavior");
    }
    std::string body;
    append_form_value(body, "end_behavior", end);
    append_form_value(body, "proration_behavior", proration_value(request.proration_behavior));
    append_metadata(body, request.metadata);
    if (request.default_automatic_tax_enabled)
        append_form_value(body,
                          "default_settings[automatic_tax][enabled]",
                          *request.default_automatic_tax_enabled ? "true" : "false");
    for (std::size_t i = 0; i < request.phases.size(); ++i) {
        const auto& phase = request.phases[i];
        if (phase.start_date <= 0 || phase.end_date.has_value() == phase.duration.has_value() ||
            (phase.end_date && *phase.end_date <= phase.start_date) ||
            (phase.duration &&
             (phase.duration->interval_count <= 0 ||
              (phase.duration->interval != "day" && phase.duration->interval != "week" &&
               phase.duration->interval != "month" && phase.duration->interval != "year"))) ||
            phase.items.empty() || phase.items.size() > 20)
            throw ConfigurationException("Invalid schedule phase dates, duration, or items");
        if (i > 0) {
            const auto& previous = request.phases[i - 1];
            if (phase.start_date <= previous.start_date ||
                (previous.end_date && *previous.end_date != phase.start_date))
                throw ConfigurationException("Schedule phase dates must be ordered and contiguous");
        }
        const auto prefix = "phases[" + std::to_string(i) + "]";
        append_form_value(body, prefix + "[start_date]", std::to_string(phase.start_date));
        if (phase.end_date)
            append_form_value(body, prefix + "[end_date]", std::to_string(*phase.end_date));
        if (phase.duration) {
            append_form_value(body, prefix + "[duration][interval]", phase.duration->interval);
            append_form_value(body,
                              prefix + "[duration][interval_count]",
                              std::to_string(phase.duration->interval_count));
        }
        append_form_value(
            body, prefix + "[proration_behavior]", proration_value(phase.proration_behavior));
        if (phase.automatic_tax_enabled)
            append_form_value(body,
                              prefix + "[automatic_tax][enabled]",
                              *phase.automatic_tax_enabled ? "true" : "false");
        for (const auto& [key, value] : phase.metadata)
            append_form_value(body, prefix + "[metadata][" + key + "]", value);
        for (std::size_t j = 0; j < phase.discounts.size(); ++j)
            append_schedule_discount(
                body, prefix + "[discounts][" + std::to_string(j) + "]", phase.discounts[j]);
        for (std::size_t j = 0; j < phase.items.size(); ++j) {
            const auto& item = phase.items[j];
            if (item.price_id.empty() || (item.quantity && *item.quantity <= 0))
                throw ConfigurationException(
                    "Schedule items require a price and positive quantity when supplied");
            const auto item_prefix = prefix + "[items][" + std::to_string(j) + "]";
            append_form_value(body, item_prefix + "[price]", item.price_id);
            if (item.quantity)
                append_form_value(body, item_prefix + "[quantity]", std::to_string(*item.quantity));
            for (std::size_t k = 0; k < item.discounts.size(); ++k)
                append_schedule_discount(body,
                                         item_prefix + "[discounts][" + std::to_string(k) + "]",
                                         item.discounts[k]);
        }
    }
    return body;
}

/** Encode an explicit payment policy. */
std::string payment_value(PaymentBehavior behavior) {
    switch (behavior) {
    case PaymentBehavior::AllowIncomplete:
        return "allow_incomplete";
    case PaymentBehavior::PendingIfIncomplete:
        return "pending_if_incomplete";
    case PaymentBehavior::ErrorIfIncomplete:
        return "error_if_incomplete";
    case PaymentBehavior::DefaultIncomplete:
        return "default_incomplete";
    }
    throw ConfigurationException("Invalid payment behavior");
}

/** Replace existing items with explicit quantities so Stripe cannot reset them to one. */
std::string subscription_body(const UpdateSubscriptionRequest& request) {
    if (!request.proration_behavior || !request.payment_behavior)
        throw ConfigurationException(
            "Subscription updates require proration and payment behaviors");
    if (request.items.empty() || request.items.size() > 20)
        throw ConfigurationException("Subscription updates require one through twenty items");
    std::string body;
    append_form_value(body, "proration_behavior", proration_value(*request.proration_behavior));
    append_form_value(body, "payment_behavior", payment_value(*request.payment_behavior));
    if (request.automatic_tax_enabled)
        append_form_value(
            body, "automatic_tax[enabled]", *request.automatic_tax_enabled ? "true" : "false");
    std::unordered_set<std::string> ids;
    for (std::size_t index = 0; index < request.items.size(); ++index) {
        const auto& item = request.items[index];
        if (item.item_id.empty() || item.price_id.empty() || item.quantity <= 0 ||
            !ids.insert(item.item_id).second)
            throw ConfigurationException(
                "Each subscription item needs a unique ID, price, and positive quantity");
        const auto prefix = "items[" + std::to_string(index) + "]";
        append_form_value(body, prefix + "[id]", item.item_id);
        append_form_value(body, prefix + "[price]", item.price_id);
        append_form_value(body, prefix + "[quantity]", std::to_string(item.quantity));
    }
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

/** Encode the common page controls, rejecting page sizes Stripe cannot accept. */
std::string payment_page_query(std::int64_t limit, const std::string& starting_after) {
    if (limit < 1 || limit > 100)
        throw ConfigurationException("Payment page size must be from 1 to 100");
    std::string query;
    append_form_value(query, "limit", std::to_string(limit));
    if (!starting_after.empty()) append_form_value(query, "starting_after", starting_after);
    return query;
}

/** Validate a refund and encode its amount and recovery metadata. */
std::string refund_body(const CreateRefundRequest& request) {
    if (request.charge_id.empty() || (request.amount && *request.amount <= 0))
        throw ConfigurationException(
            "Refund requires a charge and a positive amount when supplied");
    std::string body;
    append_form_value(body, "charge", request.charge_id);
    if (request.amount) append_form_value(body, "amount", std::to_string(*request.amount));
    append_metadata(body, request.metadata);
    return body;
}

/** Build a refund history URL with an optional charge filter. */
std::string refunds_path(const ListRefundsRequest& request) {
    auto query = payment_page_query(request.limit, request.starting_after);
    if (!request.charge_id.empty()) append_form_value(query, "charge", request.charge_id);
    return "/v1/refunds?" + query;
}

/** Build a subscription invoice query with bounded pagination. */
std::string invoices_path(const ListInvoicesRequest& request) {
    auto query = payment_page_query(request.limit, request.starting_after);
    if (!request.subscription_id.empty())
        append_form_value(query, "subscription", request.subscription_id);
    return "/v1/invoices?" + query;
}

/** Encode collection behavior and a positive optional resume timestamp. */
std::string pause_collection_body(const PauseSubscriptionCollectionRequest& request) {
    std::string behavior;
    switch (request.behavior) {
    case CollectionPauseBehavior::KeepAsDraft:
        behavior = "keep_as_draft";
        break;
    case CollectionPauseBehavior::MarkUncollectible:
        behavior = "mark_uncollectible";
        break;
    case CollectionPauseBehavior::Void:
        behavior = "void";
        break;
    default:
        throw ConfigurationException("Unknown collection pause behavior");
    }
    if (request.resumes_at && *request.resumes_at <= 0)
        throw ConfigurationException("Collection resume timestamp must be positive");
    std::string body;
    append_form_value(body, "pause_collection[behavior]", behavior);
    if (request.resumes_at)
        append_form_value(
            body, "pause_collection[resumes_at]", std::to_string(*request.resumes_at));
    return body;
}

/** Build a charge list URL with an optional customer filter. */
std::string charges_path(const ListChargesRequest& request) {
    auto query = payment_page_query(request.limit, request.starting_after);
    if (!request.customer_id.empty()) append_form_value(query, "customer", request.customer_id);
    return "/v1/charges?" + query;
}

/** Build an invoice allocation URL, selecting PaymentIntent allocations when requested. */
std::string invoice_payments_path(const ListInvoicePaymentsRequest& request) {
    auto query = payment_page_query(request.limit, request.starting_after);
    if (!request.payment_intent_id.empty()) {
        append_form_value(query, "payment[type]", "payment_intent");
        append_form_value(query, "payment[payment_intent]", request.payment_intent_id);
    }
    return "/v1/invoice_payments?" + query;
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
        (object["nickname"].isNull() || object["nickname"].isString()) &&
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
    price.nickname = json_value::string(object, "nickname");
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

/** Encode Checkout settings for StripeKit's supported API version. */
std::string checkout_session_body(const CreateCheckoutSessionRequest& request) {
    const std::string session_mode = request.mode.empty() ? "subscription" : request.mode;
    if ((!request.customer_id.empty() && !request.customer_email.empty()) ||
        (request.trial_period_days.has_value() &&
         (session_mode != "subscription" || *request.trial_period_days <= 0 ||
          *request.trial_period_days > 730)) ||
        (!request.billing_address_collection.empty() &&
         request.billing_address_collection != "auto" &&
         request.billing_address_collection != "required")) {
        throw ConfigurationException(
            "Checkout customer or subscription trial settings are invalid");
    }
    if (request.customer_id.empty() &&
        (request.customer_update_address.has_value() || request.customer_update_name.has_value()))
        throw ConfigurationException("Checkout customer updates require an existing Customer");
    std::string body;
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
    if (!request.customer_email.empty())
        append_form_value(body, "customer_email", request.customer_email);
    if (request.trial_period_days.has_value())
        append_form_value(body,
                          "subscription_data[trial_period_days]",
                          std::to_string(*request.trial_period_days));
    for (const auto& [key, value] : request.subscription_metadata)
        append_form_value(body, "subscription_data[metadata][" + key + "]", value);
    if (!request.payment_method_collection.empty())
        append_form_value(body, "payment_method_collection", request.payment_method_collection);
    for (std::size_t i = 0; i < request.payment_method_types.size(); ++i)
        append_form_value(body,
                          "allowed_payment_method_types[" + std::to_string(i) + "]",
                          request.payment_method_types[i]);
    if (request.expires_at.has_value())
        append_form_value(body, "expires_at", std::to_string(*request.expires_at));
    if (request.automatic_tax_enabled)
        append_form_value(
            body, "automatic_tax[enabled]", *request.automatic_tax_enabled ? "true" : "false");
    if (!request.billing_address_collection.empty())
        append_form_value(body, "billing_address_collection", request.billing_address_collection);
    if (request.customer_update_address)
        append_form_value(
            body, "customer_update[address]", *request.customer_update_address ? "auto" : "never");
    if (request.customer_update_name)
        append_form_value(
            body, "customer_update[name]", *request.customer_update_name ? "auto" : "never");
    if (request.tax_id_collection_enabled)
        append_form_value(body,
                          "tax_id_collection[enabled]",
                          *request.tax_id_collection_enabled ? "true" : "false");
    return body;
}

/** Validate and serialize draft invoice tax and collection changes. */
std::string invoice_update_body(const UpdateInvoiceRequest& request) {
    if (!request.automatic_tax_enabled && !request.auto_advance)
        throw ConfigurationException("Invoice updates require at least one setting");
    std::string body;
    if (request.automatic_tax_enabled)
        append_form_value(
            body, "automatic_tax[enabled]", *request.automatic_tax_enabled ? "true" : "false");
    if (request.auto_advance)
        append_form_value(body, "auto_advance", *request.auto_advance ? "true" : "false");
    return body;
}

/** Serialize customer fields without changing address fields the caller omitted. */
std::string customer_update_body(const UpdateCustomerRequest& request) {
    std::string body;
    if (request.email) append_form_value(body, "email", *request.email);
    if (request.name) append_form_value(body, "name", *request.name);
    if (request.validate_tax_location) {
        std::string validation;
        switch (*request.validate_tax_location) {
        case TaxLocationValidation::Automatic:
            validation = "auto";
            break;
        case TaxLocationValidation::Deferred:
            validation = "deferred";
            break;
        case TaxLocationValidation::Immediately:
            validation = "immediately";
            break;
        default:
            throw ConfigurationException("Invalid customer tax-location validation policy");
        }
        append_form_value(body, "tax[validate_location]", validation);
    }
    if (request.address) {
        if (request.address->country && !request.address->country->empty() &&
            (request.address->country->size() != 2 ||
             !std::all_of(request.address->country->begin(),
                          request.address->country->end(),
                          [](unsigned char ch) { return std::isalpha(ch) != 0; })))
            throw ConfigurationException("Customer address country must be a two-letter code");
        const auto append = [&](const char* key, const std::optional<std::string>& value) {
            if (value) append_form_value(body, std::string("address[") + key + "]", *value);
        };
        append("line1", request.address->line1);
        append("line2", request.address->line2);
        append("city", request.address->city);
        append("state", request.address->state);
        append("postal_code", request.address->postal_code);
        append("country", request.address->country);
    }
    if (body.empty()) throw ConfigurationException("Customer updates require at least one field");
    return body;
}

/** Return the Stripe name of a portal flow type. */
std::string portal_flow_type_value(BillingPortalFlowType type) {
    switch (type) {
    case BillingPortalFlowType::PaymentMethodUpdate:
        return "payment_method_update";
    case BillingPortalFlowType::SubscriptionCancel:
        return "subscription_cancel";
    case BillingPortalFlowType::SubscriptionUpdate:
        return "subscription_update";
    case BillingPortalFlowType::SubscriptionUpdateConfirm:
        return "subscription_update_confirm";
    }
    throw ConfigurationException("Invalid billing portal flow type");
}

/** Return the Stripe name of a portal after-completion behavior. */
std::string portal_after_completion_value(BillingPortalAfterCompletion value) {
    switch (value) {
    case BillingPortalAfterCompletion::PortalHomepage:
        return "portal_homepage";
    case BillingPortalAfterCompletion::HostedConfirmation:
        return "hosted_confirmation";
    case BillingPortalAfterCompletion::Redirect:
        return "redirect";
    }
    throw ConfigurationException("Invalid billing portal after-completion behavior");
}

/** Check that a portal flow has the details its type needs, then encode it as flow_data. */
void append_portal_flow(std::string& body, const BillingPortalFlow& flow) {
    const auto type = portal_flow_type_value(flow.type);
    const bool confirm = flow.type == BillingPortalFlowType::SubscriptionUpdateConfirm;
    const bool needs_subscription = flow.type != BillingPortalFlowType::PaymentMethodUpdate;
    if (needs_subscription == flow.subscription_id.empty())
        throw ConfigurationException(needs_subscription
                                         ? "This billing portal flow requires a subscription ID"
                                         : "A payment method flow does not take a subscription ID");
    if (confirm != !flow.items.empty())
        throw ConfigurationException(
            "Only a subscription update confirm flow takes subscription items");
    append_form_value(body, "flow_data[type]", type);
    if (needs_subscription)
        append_form_value(body, "flow_data[" + type + "][subscription]", flow.subscription_id);
    if (confirm) {
        if (flow.items.size() != 1)
            throw ConfigurationException("Stripe can confirm a change to only one item");
        const auto& item = flow.items.front();
        if (item.item_id.empty() || (item.price_id.empty() && !item.quantity) ||
            (item.quantity && *item.quantity <= 0))
            throw ConfigurationException(
                "The confirmed item needs an ID and a new price or positive quantity");
        const std::string prefix = "flow_data[subscription_update_confirm][items][0]";
        append_form_value(body, prefix + "[id]", item.item_id);
        if (!item.price_id.empty()) append_form_value(body, prefix + "[price]", item.price_id);
        if (item.quantity)
            append_form_value(body, prefix + "[quantity]", std::to_string(*item.quantity));
    }

    const auto after = flow.after_completion;
    const bool redirect = after == BillingPortalAfterCompletion::Redirect;
    const bool hosted = after == BillingPortalAfterCompletion::HostedConfirmation;
    if (redirect == flow.redirect_url.empty())
        throw ConfigurationException(redirect
                                         ? "A redirect after completion requires a redirect URL"
                                         : "A redirect URL requires a redirect after completion");
    if (!flow.confirmation_message.empty() && !hosted)
        throw ConfigurationException("A confirmation message requires a hosted confirmation");
    if (flow.confirmation_message.size() > 500)
        throw ConfigurationException("A confirmation message is limited to 500 characters");
    if (!after) return;
    append_form_value(
        body, "flow_data[after_completion][type]", portal_after_completion_value(*after));
    if (redirect)
        append_form_value(
            body, "flow_data[after_completion][redirect][return_url]", flow.redirect_url);
    if (!flow.confirmation_message.empty())
        append_form_value(body,
                          "flow_data[after_completion][hosted_confirmation][custom_message]",
                          flow.confirmation_message);
}

/** Validate and encode a portal session, including any configuration, locale, and flow. */
std::string billing_portal_session_body(const CreateBillingPortalSessionRequest& request) {
    if (request.customer_id.empty())
        throw ConfigurationException("A billing portal session requires a customer ID");
    std::string body;
    append_form_value(body, "customer", request.customer_id);
    if (!request.return_url.empty()) append_form_value(body, "return_url", request.return_url);
    if (!request.configuration_id.empty())
        append_form_value(body, "configuration", request.configuration_id);
    if (!request.locale.empty()) append_form_value(body, "locale", request.locale);
    if (request.flow) append_portal_flow(body, *request.flow);
    return body;
}

/** Reject a value Stripe would not accept for a portal feature setting. */
void require_one_of(const std::string& value,
                    std::initializer_list<std::string_view> allowed,
                    const char* setting) {
    if (std::find(allowed.begin(), allowed.end(), value) == allowed.end())
        throw ConfigurationException(std::string("Invalid billing portal ") + setting + ": " +
                                     value);
}

/**
 * Encode a list setting. An empty list is sent as an empty value on updates,
 * which is how Stripe clears a list.
 */
void append_portal_list(std::string& body,
                        const std::string& name,
                        const std::vector<std::string>& values,
                        bool clear_when_empty) {
    if (values.empty() && clear_when_empty) append_form_value(body, name, "");
    for (const auto& value : values)
        append_form_value(body, name + "[]", value);
}

/** Validate and encode every portal feature, so unset features are turned off. */
void append_portal_features(std::string& body,
                            const BillingPortalFeatures& features,
                            bool updating) {
    const auto enabled = [](bool value) { return value ? "true" : "false"; };
    const auto& customer = features.customer_update;
    for (const auto& value : customer.allowed_updates)
        require_one_of(value,
                       { "address", "email", "name", "phone", "shipping", "tax_id" },
                       "customer update");
    append_form_value(body, "features[customer_update][enabled]", enabled(customer.enabled));
    if (customer.enabled || !customer.allowed_updates.empty())
        append_portal_list(
            body, "features[customer_update][allowed_updates]", customer.allowed_updates, updating);
    append_form_value(
        body, "features[invoice_history][enabled]", enabled(features.invoice_history_enabled));
    append_form_value(body,
                      "features[payment_method_update][enabled]",
                      enabled(features.payment_method_update_enabled));

    const auto& cancel = features.subscription_cancel;
    if (!cancel.mode.empty())
        require_one_of(cancel.mode, { "at_period_end", "immediately" }, "cancellation mode");
    if (!cancel.proration_behavior.empty())
        require_one_of(cancel.proration_behavior,
                       { "none", "create_prorations" },
                       "cancellation proration behavior");
    append_form_value(body, "features[subscription_cancel][enabled]", enabled(cancel.enabled));
    if (!cancel.mode.empty())
        append_form_value(body, "features[subscription_cancel][mode]", cancel.mode);
    if (!cancel.proration_behavior.empty())
        append_form_value(
            body, "features[subscription_cancel][proration_behavior]", cancel.proration_behavior);

    const auto& update = features.subscription_update;
    for (const auto& value : update.default_allowed_updates)
        require_one_of(value, { "price", "quantity", "promotion_code" }, "plan change option");
    if (!update.proration_behavior.empty())
        require_one_of(update.proration_behavior,
                       { "none", "create_prorations", "always_invoice" },
                       "plan change proration behavior");
    if (update.products.size() > 10)
        throw ConfigurationException("Stripe allows at most 10 billing portal plan products");
    for (const auto& product : update.products)
        if (product.product_id.empty() || product.price_ids.empty() ||
            std::any_of(product.price_ids.begin(),
                        product.price_ids.end(),
                        [](const std::string& id) { return id.empty(); }))
            throw ConfigurationException(
                "Each billing portal plan product needs an ID and at least one price ID");
    if (update.enabled && (update.default_allowed_updates.empty() || update.products.empty()))
        throw ConfigurationException(
            "Enabling billing portal plan changes requires allowed changes and products");
    if (update.enabled && !features.payment_method_update_enabled)
        throw ConfigurationException(
            "Billing portal plan changes require payment method updates to be enabled");
    append_form_value(body, "features[subscription_update][enabled]", enabled(update.enabled));
    if (update.enabled || !update.default_allowed_updates.empty())
        append_portal_list(body,
                           "features[subscription_update][default_allowed_updates]",
                           update.default_allowed_updates,
                           updating);
    if (update.products.empty() && updating && update.enabled)
        append_form_value(body, "features[subscription_update][products]", "");
    for (std::size_t index = 0; index < update.products.size(); ++index) {
        const auto prefix =
            "features[subscription_update][products][" + std::to_string(index) + "]";
        append_form_value(body, prefix + "[product]", update.products[index].product_id);
        for (const auto& price : update.products[index].price_ids)
            append_form_value(body, prefix + "[prices][]", price);
    }
    if (!update.proration_behavior.empty())
        append_form_value(
            body, "features[subscription_update][proration_behavior]", update.proration_behavior);
}

/** Reject portal configuration names Stripe would refuse. */
void require_portal_name(const std::string& name) {
    if (name.size() > 256)
        throw ConfigurationException(
            "A billing portal configuration name is limited to 256 characters");
}

/**
 * Ask Stripe to include the plan-switching product list, which it otherwise
 * leaves out of portal configuration responses.
 */
void append_portal_products_expansion(std::string& encoded) {
    append_form_value(encoded, "expand[]", "features.subscription_update.products");
}

/** Build the retrieval path for a portal configuration, including its product list. */
std::string portal_configuration_path(const std::string& id) {
    std::string query;
    append_portal_products_expansion(query);
    return catalog_path("billing_portal/configurations", id) + "?" + query;
}

/** Validate and encode a new portal configuration. */
std::string portal_configuration_body(const CreateBillingPortalConfigurationRequest& request) {
    require_portal_name(request.name);
    std::string body;
    append_portal_products_expansion(body);
    append_portal_features(body, request.features, false);
    if (!request.name.empty()) append_form_value(body, "name", request.name);
    if (!request.headline.empty())
        append_form_value(body, "business_profile[headline]", request.headline);
    if (!request.default_return_url.empty())
        append_form_value(body, "default_return_url", request.default_return_url);
    append_metadata(body, request.metadata);
    return body;
}

/** Validate and encode only the portal configuration fields being changed. */
std::string portal_configuration_body(const UpdateBillingPortalConfigurationRequest& request) {
    std::string body;
    append_portal_products_expansion(body);
    if (request.active) append_form_value(body, "active", *request.active ? "true" : "false");
    if (request.features) append_portal_features(body, *request.features, true);
    if (request.name) {
        require_portal_name(*request.name);
        append_form_value(body, "name", *request.name);
    }
    if (request.headline) append_form_value(body, "business_profile[headline]", *request.headline);
    if (request.default_return_url)
        append_form_value(body, "default_return_url", *request.default_return_url);
    append_metadata(body, request.metadata);
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

namespace {

/** Read an optional Stripe Unix timestamp without accepting malformed values. */
std::optional<std::int64_t> optional_unix_timestamp(const Json::Value& value) {
    if (value.isNull()) return std::nullopt;
    if (!value.isInt64() || value.asInt64() < 0)
        throw ConfigurationException("Stripe returned an invalid subscription timestamp");
    return value.asInt64();
}

/** Read a nullable Stripe reference as an ID, accepting expanded objects. */
std::string reference_id(const Json::Value& value) {
    if (value.isNull()) return {};
    if (value.isString() && !value.asString().empty()) return value.asString();
    if (value.isObject() && value["id"].isString() && !value["id"].asString().empty())
        return value["id"].asString();
    throw ConfigurationException("Stripe returned a malformed object reference");
}

/** Read discount or tax rate IDs; reject malformed settings rather than treating them as absent. */
std::vector<std::string> reference_ids(const Json::Value& value) {
    std::vector<std::string> ids;
    if (value.isNull()) return ids;
    if (!value.isArray())
        throw ConfigurationException("Stripe returned malformed discount or tax references");
    for (const auto& entry : value) {
        auto id = reference_id(entry);
        if (id.empty())
            throw ConfigurationException("Stripe returned an empty discount or tax reference");
        ids.push_back(std::move(id));
    }
    return ids;
}

/** Read the automatic-tax fields shared by Checkout, invoices, subscriptions, and schedules. */
AutomaticTax automatic_tax_value(const Json::Value& value) {
    if (!value.isObject() || !value["enabled"].isBool())
        throw ConfigurationException("Stripe returned malformed automatic tax settings");
    if ((!value["status"].isNull() && !value["status"].isString()) ||
        (!value["disabled_reason"].isNull() && !value["disabled_reason"].isString()))
        throw ConfigurationException("Stripe returned malformed automatic tax status");
    return { .enabled = value["enabled"].asBool(),
             .status = json_value::string(value, "status"),
             .disabled_reason = json_value::string(value, "disabled_reason") };
}

/** Read a list of purchased line items without guessing a missing quantity or Price. */
std::vector<PurchasedLineItem>
purchased_line_items(const Json::Value& value, bool require_pagination, bool& has_more) {
    if (!value.isObject() || !value["data"].isArray() ||
        (require_pagination && !value["has_more"].isBool()))
        throw ConfigurationException("Stripe returned malformed purchased line items");
    if (value["has_more"].isBool()) has_more = value["has_more"].asBool();
    std::vector<PurchasedLineItem> items;
    for (const auto& entry : value["data"]) {
        if (!entry.isObject() ||
            (!entry["quantity"].isNull() &&
             (!entry["quantity"].isInt64() || entry["quantity"].asInt64() < 0)) ||
            (!entry["amount"].isNull() && !entry["amount"].isInt64()) ||
            (!entry["amount_total"].isNull() && !entry["amount_total"].isInt64()))
            throw ConfigurationException("Stripe returned a malformed purchased line item");
        auto price_id = reference_id(entry["price"]);
        if (price_id.empty() && entry["pricing"].isObject())
            price_id = reference_id(entry["pricing"]["price_details"]["price"]);
        if (price_id.empty())
            throw ConfigurationException("Stripe returned a purchased item without a Price");
        const auto id = json_value::string(entry, "id");
        if (require_pagination && id.empty())
            throw ConfigurationException("Stripe returned a line item without its pagination ID");
        PurchasedLineItem item{ .id = id, .price_id = std::move(price_id) };
        const auto& amount = entry["amount"].isNull() ? entry["amount_total"] : entry["amount"];
        if (amount.isInt64()) item.amount = amount.asInt64();
        if (!entry["quantity"].isNull()) item.quantity = entry["quantity"].asInt64();
        items.push_back(std::move(item));
    }
    return items;
}

/** Read discounts returned for a schedule phase, retaining reusable IDs. */
std::vector<SubscriptionScheduleDiscount> schedule_discounts(const Json::Value& value) {
    if (value.isNull()) return {};
    if (!value.isArray())
        throw ConfigurationException("Stripe returned malformed schedule discounts");
    std::vector<SubscriptionScheduleDiscount> discounts;
    for (const auto& entry : value) {
        if (!entry.isObject())
            throw ConfigurationException("Stripe returned a malformed schedule discount");
        SubscriptionScheduleDiscount discount;
        const auto read_reusable_reference = [&](const Json::Value& source, const char* field) {
            if (source.isObject() && source.isMember(field)) return reference_id(source[field]);
            return std::string{};
        };
        discount.coupon_id = read_reusable_reference(entry, "coupon");
        discount.promotion_code_id = read_reusable_reference(entry, "promotion_code");
        const auto& discount_value = entry["discount"];
        if (discount_value.isObject()) {
            if (discount.coupon_id.empty())
                discount.coupon_id = read_reusable_reference(discount_value, "coupon");
            if (discount.promotion_code_id.empty())
                discount.promotion_code_id =
                    read_reusable_reference(discount_value, "promotion_code");
            const auto& source = discount_value["source"];
            if (discount.coupon_id.empty())
                discount.coupon_id = read_reusable_reference(source, "coupon");
            if (discount.promotion_code_id.empty())
                discount.promotion_code_id = read_reusable_reference(source, "promotion_code");
        } else if (discount_value.isString()) {
            discount.discount_id = reference_id(discount_value);
        }
        if (discount.discount_id.empty() && discount.coupon_id.empty() &&
            discount.promotion_code_id.empty())
            discount.discount_id = reference_id(entry);
        if (discount.discount_id.empty() && discount.coupon_id.empty() &&
            discount.promotion_code_id.empty())
            throw ConfigurationException("Stripe returned a schedule discount without an ID");
        discounts.push_back(std::move(discount));
    }
    return discounts;
}

/** Read current or pending subscription items without inventing a quantity. */
std::vector<SubscriptionItem> subscription_items(const Json::Value& data) {
    if (!data.isArray())
        throw ConfigurationException("Stripe returned malformed subscription items");
    std::vector<SubscriptionItem> items;
    for (const auto& value : data) {
        if (!value.isObject() || !value["id"].isString() || value["id"].asString().empty() ||
            (!value["quantity"].isNull() &&
             (!value["quantity"].isInt64() || value["quantity"].asInt64() < 0)))
            throw ConfigurationException("Stripe returned a malformed subscription item");
        SubscriptionItem item;
        item.id = value["id"].asString();
        item.price_id = reference_id(value["price"]);
        if (item.price_id.empty())
            throw ConfigurationException("Stripe returned a subscription item without a price");
        if (!value["quantity"].isNull()) item.quantity = value["quantity"].asInt64();
        item.current_period_start_unix = optional_unix_timestamp(value["current_period_start"]);
        item.current_period_end_unix = optional_unix_timestamp(value["current_period_end"]);
        item.discount_ids = reference_ids(value["discounts"]);
        item.tax_rate_ids = reference_ids(value["tax_rates"]);
        items.push_back(std::move(item));
    }
    return items;
}

} // namespace

SubscriptionSchedule
StripeResponseParser::parse_subscription_schedule(const HttpResponse& response) const {
    const auto value = json_value::parse(response.body);
    if (!value.isObject() || value["object"] != "subscription_schedule" ||
        !value["id"].isString() || value["id"].asString().empty() || !value["status"].isString() ||
        value["status"].asString().empty() || !value["end_behavior"].isString() ||
        value["end_behavior"].asString().empty() || !value["phases"].isArray())
        throw ConfigurationException("Stripe returned a malformed subscription schedule");
    SubscriptionSchedule schedule;
    schedule.id = value["id"].asString();
    schedule.status = value["status"].asString();
    schedule.end_behavior = value["end_behavior"].asString();
    schedule.customer_id = reference_id(value["customer"]);
    if (schedule.customer_id.empty())
        throw ConfigurationException("Stripe returned a schedule without a customer");
    schedule.subscription_id = reference_id(value["subscription"]);
    schedule.released_subscription_id = reference_id(value["released_subscription"]);
    schedule.metadata = metadata_value(value);
    const auto& default_settings = value["default_settings"];
    if (!default_settings.isNull()) {
        if (!default_settings.isObject())
            throw ConfigurationException("Stripe returned malformed schedule defaults");
        const auto& automatic_tax = default_settings["automatic_tax"];
        if (!automatic_tax.isNull())
            schedule.default_automatic_tax = automatic_tax_value(automatic_tax);
    }
    const auto dates = [](const Json::Value& entry) {
        if (!entry.isObject() || !entry["start_date"].isInt64() || !entry["end_date"].isInt64() ||
            entry["start_date"].asInt64() <= 0 ||
            entry["end_date"].asInt64() <= entry["start_date"].asInt64())
            throw ConfigurationException("Stripe returned malformed schedule phase dates");
        return SubscriptionScheduleCurrentPhase{ entry["start_date"].asInt64(),
                                                 entry["end_date"].asInt64() };
    };
    if (!value["current_phase"].isNull()) schedule.current_phase = dates(value["current_phase"]);
    for (const auto& entry : value["phases"]) {
        const auto period = dates(entry);
        if (!entry["items"].isArray() || entry["items"].empty())
            throw ConfigurationException("Stripe returned a schedule phase without items");
        if (!entry["proration_behavior"].isString())
            throw ConfigurationException(
                "Stripe returned a schedule phase without a valid proration behavior");
        SubscriptionSchedulePhase phase;
        phase.start_date_unix = period.start_date_unix;
        phase.end_date_unix = period.end_date_unix;
        phase.proration_behavior = entry["proration_behavior"].asString();
        phase.metadata = metadata_value(entry);
        const auto& automatic_tax = entry["automatic_tax"];
        if (!automatic_tax.isNull()) phase.automatic_tax = automatic_tax_value(automatic_tax);
        phase.discounts = schedule_discounts(entry["discounts"]);
        for (const auto& item : entry["items"]) {
            if (!item.isObject())
                throw ConfigurationException("Stripe returned a malformed schedule item");
            SubscriptionScheduleItem parsed;
            parsed.price_id = reference_id(item["price"]);
            if (parsed.price_id.empty() ||
                (!item["quantity"].isNull() &&
                 (!item["quantity"].isInt64() || item["quantity"].asInt64() < 0)))
                throw ConfigurationException(
                    "Stripe returned a malformed schedule price or quantity");
            if (!item["quantity"].isNull()) parsed.quantity = item["quantity"].asInt64();
            parsed.discounts = schedule_discounts(item["discounts"]);
            phase.items.push_back(std::move(parsed));
        }
        schedule.phases.push_back(std::move(phase));
    }
    return schedule;
}

Price StripeResponseParser::parse_price(const HttpResponse& response) const {
    return JsonStripeResponseParser{}.parse_price(response);
}

Charge StripeResponseParser::parse_charge(const HttpResponse& response) const {
    const auto value = json_value::parse(response.body);
    if (!value.isObject() || value["object"] != "charge" || !value["id"].isString() ||
        value["id"].asString().empty() || !value["status"].isString() ||
        value["status"].asString().empty() || !value["currency"].isString() ||
        value["currency"].asString().empty() || !value["amount"].isInt64() ||
        value["amount"].asInt64() < 0 || !value["amount_refunded"].isInt64() ||
        value["amount_refunded"].asInt64() < 0 || !value["paid"].isBool() ||
        !value["disputed"].isBool() || !value["livemode"].isBool())
        throw ConfigurationException("Stripe returned a malformed charge");
    Charge charge;
    charge.id = value["id"].asString();
    charge.customer_id = reference_id(value["customer"]);
    charge.payment_intent_id = reference_id(value["payment_intent"]);
    charge.invoice_id = reference_id(value["invoice"]);
    charge.status = value["status"].asString();
    charge.currency = value["currency"].asString();
    charge.amount = value["amount"].asInt64();
    charge.amount_refunded = value["amount_refunded"].asInt64();
    charge.paid = value["paid"].asBool();
    charge.disputed = value["disputed"].asBool();
    charge.livemode = value["livemode"].asBool();
    return charge;
}

Refund StripeResponseParser::parse_refund(const HttpResponse& response) const {
    const auto value = json_value::parse(response.body);
    if (!value.isObject() || value["object"] != "refund" || !value["id"].isString() ||
        value["id"].asString().empty() || !value["status"].isString() ||
        value["status"].asString().empty() || !value["currency"].isString() ||
        value["currency"].asString().empty() || !value["amount"].isInt64() ||
        value["amount"].asInt64() <= 0 || !value["metadata"].isObject() ||
        (!value["failure_reason"].isNull() && !value["failure_reason"].isString()))
        throw ConfigurationException("Stripe returned a malformed refund");
    Refund refund;
    refund.id = value["id"].asString();
    refund.charge_id = reference_id(value["charge"]);
    refund.payment_intent_id = reference_id(value["payment_intent"]);
    if (refund.charge_id.empty() && refund.payment_intent_id.empty())
        throw ConfigurationException("Stripe returned a refund without a payment reference");
    refund.amount = value["amount"].asInt64();
    refund.currency = value["currency"].asString();
    refund.status = value["status"].asString();
    if (!value["failure_reason"].isNull())
        refund.failure_reason = value["failure_reason"].asString();
    for (const auto& key : value["metadata"].getMemberNames()) {
        if (!value["metadata"][key].isString())
            throw ConfigurationException("Stripe returned malformed refund metadata");
        refund.metadata.emplace(key, value["metadata"][key].asString());
    }
    return refund;
}

InvoicePayment StripeResponseParser::parse_invoice_payment(const HttpResponse& response) const {
    const auto value = json_value::parse(response.body);
    if (!value.isObject() || value["object"] != "invoice_payment" || !value["id"].isString() ||
        value["id"].asString().empty() || !value["status"].isString() ||
        value["status"].asString().empty() || !value["currency"].isString() ||
        value["currency"].asString().empty() || !value["amount_requested"].isInt64() ||
        value["amount_requested"].asInt64() < 0 ||
        (!value["amount_paid"].isNull() &&
         (!value["amount_paid"].isInt64() || value["amount_paid"].asInt64() < 0)) ||
        !value["is_default"].isBool() || !value["livemode"].isBool() ||
        !value["payment"].isObject() || !value["payment"]["type"].isString() ||
        value["payment"]["type"].asString().empty())
        throw ConfigurationException("Stripe returned a malformed invoice payment");
    InvoicePayment payment;
    payment.id = value["id"].asString();
    payment.invoice_id = reference_id(value["invoice"]);
    if (payment.invoice_id.empty())
        throw ConfigurationException("Stripe returned an invoice payment without an invoice");
    payment.payment_type = value["payment"]["type"].asString();
    payment.payment_intent_id = reference_id(value["payment"]["payment_intent"]);
    payment.charge_id = reference_id(value["payment"]["charge"]);
    payment.payment_record_id = reference_id(value["payment"]["payment_record"]);
    if ((payment.payment_type == "payment_intent" && payment.payment_intent_id.empty()) ||
        (payment.payment_type == "charge" && payment.charge_id.empty()) ||
        (payment.payment_type == "payment_record" && payment.payment_record_id.empty()))
        throw ConfigurationException("Stripe invoice payment reference is missing");
    payment.status = value["status"].asString();
    payment.currency = value["currency"].asString();
    payment.amount_requested = value["amount_requested"].asInt64();
    if (!value["amount_paid"].isNull()) payment.amount_paid = value["amount_paid"].asInt64();
    payment.is_default = value["is_default"].asBool();
    payment.livemode = value["livemode"].asBool();
    return payment;
}

namespace {

/** Decode one payment page, using the supplied entry decoder and rejecting unusable cursors. */
template <typename Page, typename Decoder>
Page parse_payment_page(const HttpResponse& response, Decoder decode) {
    const auto value = json_value::parse(response.body);
    if (!value.isObject() || value["object"] != "list" || !value["data"].isArray() ||
        !value["has_more"].isBool())
        throw ConfigurationException("Stripe returned a malformed payment list");
    Page result;
    result.has_more = value["has_more"].asBool();
    std::unordered_set<std::string> ids;
    for (const auto& entry : value["data"]) {
        auto item = response;
        Json::StreamWriterBuilder writer;
        item.body = Json::writeString(writer, entry);
        auto parsed = decode(item);
        if (parsed.id.empty() || !ids.insert(parsed.id).second)
            throw ConfigurationException("Stripe payment list contains missing or duplicate IDs");
        result.data.push_back(std::move(parsed));
    }
    if (result.has_more && result.data.empty())
        throw ConfigurationException("Stripe payment pagination did not advance");
    return result;
}

} // namespace

ChargeList StripeResponseParser::parse_charge_list(const HttpResponse& response) const {
    return parse_payment_page<ChargeList>(
        response, [this](const HttpResponse& item) { return parse_charge(item); });
}

RefundList StripeResponseParser::parse_refund_list(const HttpResponse& response) const {
    return parse_payment_page<RefundList>(
        response, [this](const HttpResponse& item) { return parse_refund(item); });
}

InvoicePaymentList
StripeResponseParser::parse_invoice_payment_list(const HttpResponse& response) const {
    return parse_payment_page<InvoicePaymentList>(
        response, [this](const HttpResponse& item) { return parse_invoice_payment(item); });
}

PriceList StripeResponseParser::parse_price_list(const HttpResponse& response) const {
    return JsonStripeResponseParser{}.parse_price_list(response);
}

Invoice StripeResponseParser::parse_invoice(const HttpResponse& response) const {
    return JsonStripeResponseParser{}.parse_invoice(response);
}

InvoiceList StripeResponseParser::parse_invoice_list(const HttpResponse& response) const {
    return parse_payment_page<InvoiceList>(response, [this](const HttpResponse& item) {
        const auto value = json_value::parse(item.body);
        if (!value.isObject() || value["object"] != "invoice" || !value["status"].isString() ||
            value["status"].asString().empty())
            throw ConfigurationException("Stripe returned a malformed invoice list entry");
        return parse_invoice(item);
    });
}

DeletedInvoice StripeResponseParser::parse_deleted_invoice(const HttpResponse& response) const {
    const auto value = json_value::parse(response.body);
    if (!value.isObject() || value["object"] != "invoice" || !value["id"].isString() ||
        value["id"].asString().empty() || !value["deleted"].isBool() || !value["deleted"].asBool())
        throw ConfigurationException("Stripe did not confirm invoice deletion");
    return { value["id"].asString(), true };
}

BillingPortalConfiguration
StripeResponseParser::parse_billing_portal_configuration(const HttpResponse& response) const {
    return JsonStripeResponseParser{}.parse_billing_portal_configuration(response);
}

CheckoutSessionList
StripeResponseParser::parse_checkout_session_list(const HttpResponse& response) const {
    const auto object = json_value::parse(response.body);
    if (!object.isObject() || object["object"] != "list" || !object["data"].isArray() ||
        !object["has_more"].isBool())
        throw ConfigurationException("Stripe returned a malformed Checkout session list");
    CheckoutSessionList result;
    result.has_more = object["has_more"].asBool();
    for (const auto& value : object["data"]) {
        if (!value.isObject() || value["object"] != "checkout.session" || !value["id"].isString() ||
            value["id"].asString().empty() || !value["status"].isString() ||
            value["status"].asString().empty())
            throw ConfigurationException("Stripe returned a malformed Checkout session");
        auto item = response;
        Json::StreamWriterBuilder writer;
        item.body = Json::writeString(writer, value);
        result.data.push_back(parse_checkout_session(item));
    }
    if (result.has_more && result.data.empty())
        throw ConfigurationException("Stripe Checkout pagination did not advance");
    return result;
}

PurchasedLineItemList
StripeResponseParser::parse_line_item_list(const HttpResponse& response) const {
    const auto object = json_value::parse(response.body);
    if (!object.isObject() || object["object"] != "list" || !object["has_more"].isBool())
        throw ConfigurationException("Stripe returned a malformed Checkout line-item list");
    PurchasedLineItemList result;
    result.has_more = object["has_more"].asBool();
    result.data = purchased_line_items(object, true, result.has_more);
    if (result.has_more && result.data.empty())
        throw ConfigurationException("Stripe Checkout line-item pagination did not advance");
    return result;
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
    product.default_price_id = reference_id(object["default_price"]);
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
    Customer customer{ .id = json_value::string(object, "id"),
                       .email = json_value::string(object, "email"),
                       .name = json_value::string(object, "name"),
                       .livemode = json_value::boolean(object, "livemode"),
                       .metadata = metadata_value(object) };
    const auto& address = object["address"];
    if (!address.isNull()) {
        if (!address.isObject())
            throw ConfigurationException("Stripe returned a malformed customer address");
        BillingAddress parsed;
        const auto read = [&](const char* field) -> std::optional<std::string> {
            if (address[field].isNull()) return std::nullopt;
            if (!address[field].isString())
                throw ConfigurationException("Stripe returned a malformed customer address");
            return address[field].asString();
        };
        parsed.line1 = read("line1");
        parsed.line2 = read("line2");
        parsed.city = read("city");
        parsed.state = read("state");
        parsed.postal_code = read("postal_code");
        parsed.country = read("country");
        customer.address = std::move(parsed);
    }
    const auto& tax = object["tax"];
    if (!tax.isNull()) {
        if (!tax.isObject())
            throw ConfigurationException("Stripe returned malformed customer tax details");
        const auto& location = tax["location"];
        if (!location.isNull()) {
            if (!location.isObject() ||
                (!location["status"].isNull() && !location["status"].isString()) ||
                (!location["source"].isNull() && !location["source"].isString()))
                throw ConfigurationException("Stripe returned a malformed customer tax location");
            customer.tax_location_status = json_value::string(location, "status");
            customer.tax_location_source = json_value::string(location, "source");
        }
    }
    return customer;
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

    CheckoutSession session{ .id = json_value::string(object, "id"),
                             .customer_id = string_or_object_id(object, "customer"),
                             .subscription_id = string_or_object_id(object, "subscription"),
                             .payment_intent_id = string_or_object_id(object, "payment_intent"),
                             .status = json_value::string(object, "status"),
                             .mode = json_value::string(object, "mode"),
                             .url = json_value::string(object, "url"),
                             .currency = json_value::string(object, "currency"),
                             .amount_total = json_value::integer(object, "amount_total"),
                             .livemode = json_value::boolean(object, "livemode"),
                             .metadata = metadata_value(object),
                             .expires_at_unix = json_value::integer(object, "expires_at") };
    if (object["amount_subtotal"].isInt64())
        session.amount_subtotal = object["amount_subtotal"].asInt64();
    else if (!object["amount_subtotal"].isNull())
        throw ConfigurationException("Stripe returned a malformed Checkout subtotal");
    session.payment_status = json_value::string(object, "payment_status");
    session.billing_address_collection = json_value::string(object, "billing_address_collection");
    const auto& tax_ids = object["tax_id_collection"];
    if (!tax_ids.isNull()) {
        if (!tax_ids.isObject() || !tax_ids["enabled"].isBool())
            throw ConfigurationException("Stripe returned malformed Checkout tax-ID collection");
        session.tax_id_collection_enabled = tax_ids["enabled"].asBool();
    }
    const auto& totals = object["total_details"];
    if (!totals.isNull()) {
        if (!totals.isObject() ||
            (!totals["amount_discount"].isNull() && !totals["amount_discount"].isInt64()) ||
            (!totals["amount_tax"].isNull() && !totals["amount_tax"].isInt64()))
            throw ConfigurationException("Stripe returned malformed Checkout total details");
        if (totals["amount_discount"].isInt64())
            session.amount_discount = totals["amount_discount"].asInt64();
        if (totals["amount_tax"].isInt64()) session.amount_tax = totals["amount_tax"].asInt64();
    }
    if (!object["automatic_tax"].isNull())
        session.automatic_tax = automatic_tax_value(object["automatic_tax"]);
    if (!object["line_items"].isNull())
        session.line_items =
            purchased_line_items(object["line_items"], false, session.line_items_has_more);
    return session;
}

Subscription JsonStripeResponseParser::parse_subscription(const HttpResponse& response) const {
    const auto object = json_value::parse(response.body);
    if (!object.isObject() || !object["id"].isString() || object["id"].asString().empty() ||
        !object["status"].isString() || object["status"].asString().empty())
        throw ConfigurationException("Stripe returned a malformed subscription");
    auto subscription =
        Subscription{ .id = json_value::string(object, "id"),
                      .customer_id = reference_id(object["customer"]),
                      .status = json_value::string(object, "status"),
                      .current_period_start_unix =
                          optional_unix_timestamp(object["current_period_start"]).value_or(0),
                      .current_period_end_unix =
                          optional_unix_timestamp(object["current_period_end"]).value_or(0),
                      .cancel_at_period_end = json_value::boolean(object, "cancel_at_period_end"),
                      .livemode = json_value::boolean(object, "livemode"),
                      .metadata = metadata_value(object) };
    subscription.trial_end_unix = optional_unix_timestamp(object["trial_end"]);
    subscription.schedule_id = reference_id(object["schedule"]);
    subscription.discount_ids = reference_ids(object["discounts"]);
    const auto legacy_discount = reference_id(object["discount"]);
    if (!legacy_discount.empty() && std::find(subscription.discount_ids.begin(),
                                              subscription.discount_ids.end(),
                                              legacy_discount) == subscription.discount_ids.end())
        subscription.discount_ids.push_back(legacy_discount);
    subscription.default_tax_rate_ids = reference_ids(object["default_tax_rates"]);
    const auto& automatic_tax = object["automatic_tax"];
    if (!automatic_tax.isNull()) {
        const auto tax = automatic_tax_value(automatic_tax);
        subscription.automatic_tax_enabled = tax.enabled;
        subscription.automatic_tax_disabled_reason = tax.disabled_reason;
    }
    const auto& pause = object["pause_collection"];
    if (!pause.isNull()) {
        if (!pause.isObject() || !pause["behavior"].isString() ||
            pause["behavior"].asString().empty())
            throw ConfigurationException("Stripe returned a malformed collection pause");
        subscription.pause_collection =
            SubscriptionCollectionPause{ pause["behavior"].asString(),
                                         optional_unix_timestamp(pause["resumes_at"]) };
    }
    const auto& items = object["items"];
    if (!items.isNull()) {
        if (!items.isObject() || !items["has_more"].isBool())
            throw ConfigurationException("Stripe returned a malformed subscription item list");
        subscription.items = subscription_items(items["data"]);
        subscription.items_has_more = items["has_more"].asBool();
    }
    subscription.latest_invoice_id = reference_id(object["latest_invoice"]);
    const auto& pending = object["pending_update"];
    if (!pending.isNull()) {
        if (!pending.isObject() || !pending["expires_at"].isInt64() ||
            pending["expires_at"].asInt64() <= 0)
            throw ConfigurationException("Stripe returned a malformed pending subscription update");
        PendingSubscriptionUpdate update;
        update.expires_at_unix = pending["expires_at"].asInt64();
        if (!pending["subscription_items"].isNull())
            update.items = subscription_items(pending["subscription_items"]);
        subscription.pending_update = std::move(update);
    }
    return subscription;
}

BillingPortalSession
JsonStripeResponseParser::parse_billing_portal_session(const HttpResponse& response) const {
    const auto object = json_value::parse(response.body);
    return BillingPortalSession{ .id = json_value::string(object, "id"),
                                 .customer_id = json_value::string(object, "customer"),
                                 .url = json_value::string(object, "url"),
                                 .return_url = json_value::string(object, "return_url"),
                                 .created_unix = json_value::integer(object, "created"),
                                 .livemode = json_value::boolean(object, "livemode"),
                                 .configuration_id = reference_id(object["configuration"]),
                                 .locale = json_value::string(object, "locale") };
}

namespace {

/** Read an optional list of strings, rejecting values of the wrong type. */
std::vector<std::string> string_list(const Json::Value& value) {
    std::vector<std::string> result;
    if (value.isNull()) return result;
    if (!value.isArray())
        throw ConfigurationException("Stripe returned a malformed billing portal setting");
    for (const auto& entry : value) {
        if (!entry.isString())
            throw ConfigurationException("Stripe returned a malformed billing portal setting");
        result.push_back(entry.asString());
    }
    return result;
}

/** Read the plan-switching products, given as a plain array or as a list object. */
std::vector<BillingPortalProduct> portal_products(const Json::Value& value) {
    const auto& entries = value.isObject() ? value["data"] : value;
    std::vector<BillingPortalProduct> products;
    if (entries.isNull()) return products;
    if (!entries.isArray())
        throw ConfigurationException("Stripe returned malformed billing portal products");
    for (const auto& entry : entries) {
        BillingPortalProduct product{ .product_id = reference_id(entry["product"]),
                                      .price_ids = {} };
        for (const auto& price :
             entry["prices"].isNull() ? Json::Value(Json::arrayValue) : entry["prices"])
            product.price_ids.push_back(reference_id(price));
        if (product.product_id.empty())
            throw ConfigurationException("Stripe returned malformed billing portal products");
        products.push_back(std::move(product));
    }
    return products;
}

} // namespace

Invoice JsonStripeResponseParser::parse_invoice(const HttpResponse& response) const {
    const auto object = json_value::parse(response.body);
    if (!object.isObject() || !object["id"].isString() || object["id"].asString().empty())
        throw ConfigurationException("Stripe returned a malformed invoice");
    Invoice invoice{ .id = object["id"].asString(),
                     .customer_id = reference_id(object["customer"]),
                     .subscription_id = reference_id(object["subscription"]),
                     .status = json_value::string(object, "status"),
                     .currency = json_value::string(object, "currency"),
                     .amount_due = json_value::integer(object, "amount_due"),
                     .amount_paid = json_value::integer(object, "amount_paid"),
                     .amount_remaining = json_value::integer(object, "amount_remaining"),
                     .paid = false,
                     .livemode = json_value::boolean(object, "livemode"),
                     .number = json_value::string(object, "number"),
                     .hosted_invoice_url = json_value::string(object, "hosted_invoice_url"),
                     .invoice_pdf = json_value::string(object, "invoice_pdf"),
                     .billing_reason = json_value::string(object, "billing_reason"),
                     .attempt_count = json_value::integer(object, "attempt_count"),
                     .created_unix = json_value::integer(object, "created"),
                     .due_date_unix = optional_unix_timestamp(object["due_date"]),
                     .next_payment_attempt_unix =
                         optional_unix_timestamp(object["next_payment_attempt"]) };
    // Newer Stripe API versions moved the subscription under `parent` and dropped `paid`.
    if (invoice.subscription_id.empty()) {
        const auto& parent = object["parent"];
        if (parent.isObject() && parent["subscription_details"].isObject())
            invoice.subscription_id = reference_id(parent["subscription_details"]["subscription"]);
    }
    invoice.paid = object["paid"].isBool() ? object["paid"].asBool() : invoice.status == "paid";
    const auto parse_optional_amount = [&](const char* name) -> std::optional<std::int64_t> {
        const auto& amount = object[name];
        if (amount.isNull()) return std::nullopt;
        if (!amount.isInt64())
            throw ConfigurationException(std::string("Stripe returned malformed invoice ") + name);
        return amount.asInt64();
    };
    invoice.subtotal = parse_optional_amount("subtotal");
    invoice.total = parse_optional_amount("total");
    const auto sum_amounts = [](const Json::Value& values,
                                const char* field) -> std::optional<std::int64_t> {
        if (values.isNull()) return std::nullopt;
        if (!values.isArray())
            throw ConfigurationException("Stripe returned malformed invoice amount details");
        std::int64_t sum = 0;
        for (const auto& entry : values) {
            if (!entry.isObject() || !entry[field].isInt64())
                throw ConfigurationException("Stripe returned malformed invoice amount details");
            sum += entry[field].asInt64();
        }
        return sum;
    };
    invoice.amount_discount = sum_amounts(object["total_discount_amounts"], "amount");
    invoice.amount_tax = sum_amounts(object["total_taxes"], "amount");
    if (!invoice.amount_tax) {
        const auto legacy_tax = object["total_tax"].isNull() ? object["tax"] : object["total_tax"];
        if (!legacy_tax.isNull() && !legacy_tax.isInt64())
            throw ConfigurationException("Stripe returned malformed invoice tax total");
        if (legacy_tax.isInt64()) invoice.amount_tax = legacy_tax.asInt64();
    }
    if (!object["automatic_tax"].isNull())
        invoice.automatic_tax = automatic_tax_value(object["automatic_tax"]);
    const auto& finalization_error = object["last_finalization_error"];
    if (!finalization_error.isNull()) {
        if (!finalization_error.isObject() ||
            (!finalization_error["code"].isNull() && !finalization_error["code"].isString()))
            throw ConfigurationException("Stripe returned a malformed invoice finalization error");
        invoice.last_finalization_error_code = json_value::string(finalization_error, "code");
    }
    if (!object["lines"].isNull())
        invoice.line_items =
            purchased_line_items(object["lines"], false, invoice.line_items_has_more);
    if (!object["auto_advance"].isNull()) {
        if (!object["auto_advance"].isBool())
            throw ConfigurationException("Stripe returned malformed invoice auto_advance");
        invoice.auto_advance = object["auto_advance"].asBool();
    }
    return invoice;
}

BillingPortalConfiguration
JsonStripeResponseParser::parse_billing_portal_configuration(const HttpResponse& response) const {
    const auto object = json_value::parse(response.body);
    if (!object.isObject() || !object["id"].isString() || object["id"].asString().empty() ||
        !object["features"].isObject())
        throw ConfigurationException("Stripe returned a malformed billing portal configuration");
    const auto& features = object["features"];
    const auto& customer = features["customer_update"];
    const auto& cancel = features["subscription_cancel"];
    const auto& update = features["subscription_update"];
    BillingPortalConfiguration configuration{
        .id = object["id"].asString(),
        .active = json_value::boolean(object, "active"),
        .is_default = json_value::boolean(object, "is_default"),
        .name = json_value::string(object, "name"),
        .headline = json_value::string(object["business_profile"], "headline"),
        .default_return_url = json_value::string(object, "default_return_url"),
        .features = {},
        .created_unix = json_value::integer(object, "created"),
        .livemode = json_value::boolean(object, "livemode"),
        .metadata = metadata_value(object)
    };
    // Report features Stripe leaves out as off, rather than using the request defaults.
    auto& result = configuration.features;
    result.customer_update = { .enabled = json_value::boolean(customer, "enabled"),
                               .allowed_updates = string_list(customer["allowed_updates"]) };
    result.invoice_history_enabled = json_value::boolean(features["invoice_history"], "enabled");
    result.payment_method_update_enabled =
        json_value::boolean(features["payment_method_update"], "enabled");
    result.subscription_cancel = { .enabled = json_value::boolean(cancel, "enabled"),
                                   .mode = json_value::string(cancel, "mode"),
                                   .proration_behavior =
                                       json_value::string(cancel, "proration_behavior") };
    result.subscription_update = { .enabled = json_value::boolean(update, "enabled"),
                                   .default_allowed_updates =
                                       string_list(update["default_allowed_updates"]),
                                   .products = portal_products(update["products"]),
                                   .proration_behavior =
                                       json_value::string(update, "proration_behavior") };
    return configuration;
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

Task<Customer> StripeApiClient::CustomerOperations::update_async(
    std::string customer_id, const UpdateCustomerRequest& request, const RequestOptions& options) {
    return send_and_parse_async<Customer>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Post,
                     catalog_path("customers", customer_id),
                     customer_update_body(request),
                     options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_customer(response);
        });
}

void StripeApiClient::CustomerOperations::update(std::string customer_id,
                                                 const UpdateCustomerRequest& request,
                                                 const RequestOptions& options,
                                                 RetrieveCompletion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Post,
                            catalog_path("customers", customer_id),
                            customer_update_body(request),
                            options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Customer>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_customer(response);
        },
        std::move(completion));
}

StripeApiClient::CheckoutSessionOperations::CheckoutSessionOperations(StripeApiClient& owner)
    : owner(owner) {}

Task<CheckoutSession>
StripeApiClient::CheckoutSessionOperations::expire_async(std::string id,
                                                         const RequestOptions& options) {
    return send_and_parse_async<CheckoutSession>(
        owner.http_client,
        owner.response_parser,
        make_request(
            HttpMethod::Post, catalog_path("checkout/sessions", id) + "/expire", {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_checkout_session(response);
        });
}

void StripeApiClient::CheckoutSessionOperations::expire(std::string id,
                                                        const RequestOptions& options,
                                                        CreateCompletion completion) {
    HttpRequest request;
    try {
        request = make_request(
            HttpMethod::Post, catalog_path("checkout/sessions", id) + "/expire", {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<CheckoutSession>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(request),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_checkout_session(response);
        },
        std::move(completion));
}

Task<CheckoutSessionList>
StripeApiClient::CheckoutSessionOperations::list_async(const ListCheckoutSessionsRequest& request,
                                                       const RequestOptions& options) {
    return send_and_parse_async<CheckoutSessionList>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, checkout_sessions_path(request), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_checkout_session_list(response);
        });
}

void StripeApiClient::CheckoutSessionOperations::list(const ListCheckoutSessionsRequest& filters,
                                                      const RequestOptions& options,
                                                      ListCompletion completion) {
    HttpRequest request;
    try {
        request = make_request(HttpMethod::Get, checkout_sessions_path(filters), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<CheckoutSessionList>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(request),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_checkout_session_list(response);
        },
        std::move(completion));
}

Task<PurchasedLineItemList> StripeApiClient::CheckoutSessionOperations::list_line_items_async(
    std::string session_id,
    const ListPurchasedLineItemsRequest& request,
    const RequestOptions& options) {
    return send_and_parse_async<PurchasedLineItemList>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, checkout_line_items_path(session_id, request), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_line_item_list(response);
        });
}

void StripeApiClient::CheckoutSessionOperations::list_line_items(
    std::string session_id,
    const ListPurchasedLineItemsRequest& request,
    const RequestOptions& options,
    LineItemsCompletion completion) {
    HttpRequest http;
    try {
        http = make_request(
            HttpMethod::Get, checkout_line_items_path(session_id, request), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<PurchasedLineItemList>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_line_item_list(response);
        },
        std::move(completion));
}

Task<CheckoutSession>
StripeApiClient::CheckoutSessionOperations::retrieve_async(std::string session_id,
                                                           const RequestOptions& options) {
    return send_and_parse_async<CheckoutSession>(
        owner.http_client,
        owner.response_parser,
        make_request(
            HttpMethod::Get, "/v1/checkout/sessions/" + url_encode(session_id), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_checkout_session(response);
        });
}

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
    HttpRequest http;
    try {
        http = make_request(
            HttpMethod::Post, "/v1/checkout/sessions", checkout_session_body(request), options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<CheckoutSession>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
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

Task<Subscription> StripeApiClient::SubscriptionOperations::update_async(
    std::string id, const UpdateSubscriptionRequest& request, const RequestOptions& options) {
    return send_and_parse_async<Subscription>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Post,
                     catalog_path("subscriptions", id),
                     subscription_body(request),
                     options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription(response);
        });
}

void StripeApiClient::SubscriptionOperations::update(std::string id,
                                                     const UpdateSubscriptionRequest& request,
                                                     const RequestOptions& options,
                                                     UpdateCompletion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Post,
                            catalog_path("subscriptions", id),
                            subscription_body(request),
                            options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Subscription>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription(response);
        },
        std::move(completion));
}

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
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Post,
                            "/v1/billing_portal/sessions",
                            billing_portal_session_body(request),
                            options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<BillingPortalSession>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
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

BillingPortalConfigurations& StripeApiClient::BillingPortalOperations::configurations() {
    return owner.billing_portal_configuration_operations;
}

namespace {

/** Parse a billing portal configuration response. */
BillingPortalConfiguration parse_portal_configuration(const StripeResponseParser& parser,
                                                      const HttpResponse& response) {
    return parser.parse_billing_portal_configuration(response);
}

/** Parse an invoice response. */
Invoice parse_invoice_response(const StripeResponseParser& parser, const HttpResponse& response) {
    return parser.parse_invoice(response);
}

} // namespace

StripeApiClient::InvoiceOperations::InvoiceOperations(StripeApiClient& owner) : owner(owner) {}

Task<Invoice> StripeApiClient::InvoiceOperations::retrieve_async(std::string id,
                                                                 const RequestOptions& options) {
    return send_and_parse_async<Invoice>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, catalog_path("invoices", id), {}, options),
        parse_invoice_response);
}

void StripeApiClient::InvoiceOperations::retrieve(std::string id,
                                                  const RequestOptions& options,
                                                  Completion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Get, catalog_path("invoices", id), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Invoice>(owner.http_client,
                            owner.response_parser,
                            owner.callback_lifetime,
                            std::move(http),
                            parse_invoice_response,
                            std::move(completion));
}

StripeApiClient::BillingPortalConfigurationOperations::BillingPortalConfigurationOperations(
    StripeApiClient& owner)
    : owner(owner) {}

Task<BillingPortalConfiguration>
StripeApiClient::BillingPortalConfigurationOperations::create_async(
    const CreateBillingPortalConfigurationRequest& request, const RequestOptions& options) {
    return send_and_parse_async<BillingPortalConfiguration>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Post,
                     "/v1/billing_portal/configurations",
                     portal_configuration_body(request),
                     options),
        parse_portal_configuration);
}

void StripeApiClient::BillingPortalConfigurationOperations::create(
    const CreateBillingPortalConfigurationRequest& request,
    const RequestOptions& options,
    Completion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Post,
                            "/v1/billing_portal/configurations",
                            portal_configuration_body(request),
                            options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<BillingPortalConfiguration>(owner.http_client,
                                               owner.response_parser,
                                               owner.callback_lifetime,
                                               std::move(http),
                                               parse_portal_configuration,
                                               std::move(completion));
}

Task<BillingPortalConfiguration>
StripeApiClient::BillingPortalConfigurationOperations::retrieve_async(
    std::string id, const RequestOptions& options) {
    return send_and_parse_async<BillingPortalConfiguration>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, portal_configuration_path(id), {}, options),
        parse_portal_configuration);
}

void StripeApiClient::BillingPortalConfigurationOperations::retrieve(std::string id,
                                                                     const RequestOptions& options,
                                                                     Completion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Get, portal_configuration_path(id), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<BillingPortalConfiguration>(owner.http_client,
                                               owner.response_parser,
                                               owner.callback_lifetime,
                                               std::move(http),
                                               parse_portal_configuration,
                                               std::move(completion));
}

Task<BillingPortalConfiguration>
StripeApiClient::BillingPortalConfigurationOperations::update_async(
    std::string id,
    const UpdateBillingPortalConfigurationRequest& request,
    const RequestOptions& options) {
    return send_and_parse_async<BillingPortalConfiguration>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Post,
                     catalog_path("billing_portal/configurations", id),
                     portal_configuration_body(request),
                     options),
        parse_portal_configuration);
}

void StripeApiClient::BillingPortalConfigurationOperations::update(
    std::string id,
    const UpdateBillingPortalConfigurationRequest& request,
    const RequestOptions& options,
    Completion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Post,
                            catalog_path("billing_portal/configurations", id),
                            portal_configuration_body(request),
                            options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<BillingPortalConfiguration>(owner.http_client,
                                               owner.response_parser,
                                               owner.callback_lifetime,
                                               std::move(http),
                                               parse_portal_configuration,
                                               std::move(completion));
}

StripeApiClient::StripeApiClient(HTTPClient& http_client)
    : http_client(http_client), default_response_parser(), response_parser(default_response_parser),
      callback_lifetime(std::make_shared<CallbackLifetime>()), customer_operations(*this),
      checkout_session_operations(*this), checkout_operations(*this),
      subscription_operations(*this), billing_portal_session_operations(*this),
      billing_portal_operations(*this), product_operations(*this), price_operations(*this),
      invoice_operations(*this), billing_portal_configuration_operations(*this),
      charge_operations(*this), invoice_payment_operations(*this), refund_operations(*this),
      subscription_schedule_operations(*this) {}

StripeApiClient::StripeApiClient(HTTPClient& http_client,
                                 const StripeResponseParser& response_parser)
    : http_client(http_client), default_response_parser(), response_parser(response_parser),
      callback_lifetime(std::make_shared<CallbackLifetime>()), customer_operations(*this),
      checkout_session_operations(*this), checkout_operations(*this),
      subscription_operations(*this), billing_portal_session_operations(*this),
      billing_portal_operations(*this), product_operations(*this), price_operations(*this),
      invoice_operations(*this), billing_portal_configuration_operations(*this),
      charge_operations(*this), invoice_payment_operations(*this), refund_operations(*this),
      subscription_schedule_operations(*this) {}

StripeApiClient::~StripeApiClient() {
    callback_lifetime->close_and_wait();
}

Customers& StripeApiClient::customers() {
    return customer_operations;
}

Charges& StripeApiClient::charges() {
    return charge_operations;
}
InvoicePayments& StripeApiClient::invoice_payments() {
    return invoice_payment_operations;
}

Refunds& StripeApiClient::refunds() {
    return refund_operations;
}

SubscriptionSchedules& StripeApiClient::subscription_schedules() {
    return subscription_schedule_operations;
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

Invoices& StripeApiClient::invoices() {
    return invoice_operations;
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

Task<Price> StripeApiClient::PriceOperations::update_async(std::string id,
                                                           const UpdatePriceRequest& request,
                                                           const RequestOptions& options) {
    return send_and_parse_async<Price>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Post, catalog_path("prices", id), price_body(request), options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_price(response);
        });
}

void StripeApiClient::PriceOperations::update(std::string id,
                                              const UpdatePriceRequest& request,
                                              const RequestOptions& options,
                                              Completion completion) {
    HttpRequest http;
    try {
        http = make_request(
            HttpMethod::Post, catalog_path("prices", id), price_body(request), options);
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

Task<Charge> StripeApiClient::ChargeOperations::retrieve_async(std::string id,
                                                               const RequestOptions& options) {
    return send_and_parse_async<Charge>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, catalog_path("charges", id), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_charge(response);
        });
}

void StripeApiClient::ChargeOperations::retrieve(std::string id,
                                                 const RequestOptions& options,
                                                 Completion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Get, catalog_path("charges", id), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Charge>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_charge(response);
        },
        std::move(completion));
}

Task<ChargeList> StripeApiClient::ChargeOperations::list_async(const ListChargesRequest& request,
                                                               const RequestOptions& options) {
    return send_and_parse_async<ChargeList>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, charges_path(request), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_charge_list(response);
        });
}

void StripeApiClient::ChargeOperations::list(const ListChargesRequest& request,
                                             const RequestOptions& options,
                                             ListCompletion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Get, charges_path(request), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<ChargeList>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_charge_list(response);
        },
        std::move(completion));
}

Task<InvoicePaymentList>
StripeApiClient::InvoicePaymentOperations::list_async(const ListInvoicePaymentsRequest& request,
                                                      const RequestOptions& options) {
    return send_and_parse_async<InvoicePaymentList>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, invoice_payments_path(request), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_invoice_payment_list(response);
        });
}

void StripeApiClient::InvoicePaymentOperations::list(const ListInvoicePaymentsRequest& request,
                                                     const RequestOptions& options,
                                                     ListCompletion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Get, invoice_payments_path(request), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<InvoicePaymentList>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_invoice_payment_list(response);
        },
        std::move(completion));
}

Task<Refund> StripeApiClient::RefundOperations::create_async(const CreateRefundRequest& request,
                                                             const RequestOptions& options) {
    return send_and_parse_async<Refund>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Post, "/v1/refunds", refund_body(request), options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_refund(response);
        });
}

void StripeApiClient::RefundOperations::create(const CreateRefundRequest& request,
                                               const RequestOptions& options,
                                               Completion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Post, "/v1/refunds", refund_body(request), options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Refund>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_refund(response);
        },
        std::move(completion));
}

Task<RefundList> StripeApiClient::RefundOperations::list_async(const ListRefundsRequest& request,
                                                               const RequestOptions& options) {
    return send_and_parse_async<RefundList>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, refunds_path(request), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_refund_list(response);
        });
}

void StripeApiClient::RefundOperations::list(const ListRefundsRequest& request,
                                             const RequestOptions& options,
                                             ListCompletion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Get, refunds_path(request), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<RefundList>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_refund_list(response);
        },
        std::move(completion));
}

Task<Subscription> StripeApiClient::SubscriptionOperations::set_cancel_at_period_end_async(
    std::string id, bool cancel, const RequestOptions& options) {
    return send_and_parse_async<Subscription>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Post,
                     catalog_path("subscriptions", id),
                     cancel ? "cancel_at_period_end=true" : "cancel_at_period_end=false",
                     options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription(response);
        });
}

void StripeApiClient::SubscriptionOperations::set_cancel_at_period_end(
    std::string id, bool cancel, const RequestOptions& options, UpdateCompletion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Post,
                            catalog_path("subscriptions", id),
                            cancel ? "cancel_at_period_end=true" : "cancel_at_period_end=false",
                            options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Subscription>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription(response);
        },
        std::move(completion));
}

Task<Subscription> StripeApiClient::SubscriptionOperations::pause_collection_async(
    std::string id,
    const PauseSubscriptionCollectionRequest& request,
    const RequestOptions& options) {
    return send_and_parse_async<Subscription>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Post,
                     catalog_path("subscriptions", id),
                     pause_collection_body(request),
                     options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription(response);
        });
}

void StripeApiClient::SubscriptionOperations::pause_collection(
    std::string id,
    const PauseSubscriptionCollectionRequest& request,
    const RequestOptions& options,
    UpdateCompletion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Post,
                            catalog_path("subscriptions", id),
                            pause_collection_body(request),
                            options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Subscription>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription(response);
        },
        std::move(completion));
}

Task<Subscription>
StripeApiClient::SubscriptionOperations::resume_collection_async(std::string id,
                                                                 const RequestOptions& options) {
    return send_and_parse_async<Subscription>(
        owner.http_client,
        owner.response_parser,
        make_request(
            HttpMethod::Post, catalog_path("subscriptions", id), "pause_collection=", options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription(response);
        });
}

void StripeApiClient::SubscriptionOperations::resume_collection(std::string id,
                                                                const RequestOptions& options,
                                                                UpdateCompletion completion) {
    HttpRequest http;
    try {
        http = make_request(
            HttpMethod::Post, catalog_path("subscriptions", id), "pause_collection=", options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Subscription>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription(response);
        },
        std::move(completion));
}

Task<InvoiceList> StripeApiClient::InvoiceOperations::list_async(const ListInvoicesRequest& request,
                                                                 const RequestOptions& options) {
    return send_and_parse_async<InvoiceList>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, invoices_path(request), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_invoice_list(response);
        });
}

void StripeApiClient::InvoiceOperations::list(const ListInvoicesRequest& request,
                                              const RequestOptions& options,
                                              ListCompletion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Get, invoices_path(request), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<InvoiceList>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_invoice_list(response);
        },
        std::move(completion));
}

Task<PurchasedLineItemList> StripeApiClient::InvoiceOperations::list_line_items_async(
    std::string invoice_id,
    const ListPurchasedLineItemsRequest& request,
    const RequestOptions& options) {
    return send_and_parse_async<PurchasedLineItemList>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, invoice_line_items_path(invoice_id, request), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_line_item_list(response);
        });
}

void StripeApiClient::InvoiceOperations::list_line_items(
    std::string invoice_id,
    const ListPurchasedLineItemsRequest& request,
    const RequestOptions& options,
    LineItemsCompletion completion) {
    HttpRequest http;
    try {
        http = make_request(
            HttpMethod::Get, invoice_line_items_path(invoice_id, request), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<PurchasedLineItemList>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_line_item_list(response);
        },
        std::move(completion));
}

Task<Invoice> StripeApiClient::InvoiceOperations::update_async(std::string id,
                                                               const UpdateInvoiceRequest& request,
                                                               const RequestOptions& options) {
    return send_and_parse_async<Invoice>(
        owner.http_client,
        owner.response_parser,
        make_request(
            HttpMethod::Post, catalog_path("invoices", id), invoice_update_body(request), options),
        parse_invoice_response);
}

void StripeApiClient::InvoiceOperations::update(std::string id,
                                                const UpdateInvoiceRequest& request,
                                                const RequestOptions& options,
                                                Completion completion) {
    HttpRequest http;
    try {
        http = make_request(
            HttpMethod::Post, catalog_path("invoices", id), invoice_update_body(request), options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Invoice>(owner.http_client,
                            owner.response_parser,
                            owner.callback_lifetime,
                            std::move(http),
                            parse_invoice_response,
                            std::move(completion));
}

Task<Invoice> StripeApiClient::InvoiceOperations::finalize_async(std::string id,
                                                                 const RequestOptions& options) {
    return send_and_parse_async<Invoice>(owner.http_client,
                                         owner.response_parser,
                                         make_request(HttpMethod::Post,
                                                      catalog_path("invoices", id) + "/finalize",
                                                      "auto_advance=false",
                                                      options),
                                         parse_invoice_response);
}

void StripeApiClient::InvoiceOperations::finalize(std::string id,
                                                  const RequestOptions& options,
                                                  Completion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Post,
                            catalog_path("invoices", id) + "/finalize",
                            "auto_advance=false",
                            options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Invoice>(owner.http_client,
                            owner.response_parser,
                            owner.callback_lifetime,
                            std::move(http),
                            parse_invoice_response,
                            std::move(completion));
}

Task<Invoice> StripeApiClient::InvoiceOperations::void_async(std::string id,
                                                             const RequestOptions& options) {
    return send_and_parse_async<Invoice>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Post, catalog_path("invoices", id) + "/void", {}, options),
        parse_invoice_response);
}

void StripeApiClient::InvoiceOperations::void_invoice(std::string id,
                                                      const RequestOptions& options,
                                                      Completion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Post, catalog_path("invoices", id) + "/void", {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<Invoice>(owner.http_client,
                            owner.response_parser,
                            owner.callback_lifetime,
                            std::move(http),
                            parse_invoice_response,
                            std::move(completion));
}

Task<DeletedInvoice>
StripeApiClient::InvoiceOperations::delete_async(std::string id, const RequestOptions& options) {
    return send_and_parse_async<DeletedInvoice>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Delete, catalog_path("invoices", id), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_deleted_invoice(response);
        });
}

void StripeApiClient::InvoiceOperations::delete_invoice(std::string id,
                                                        const RequestOptions& options,
                                                        DeleteCompletion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Delete, catalog_path("invoices", id), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<DeletedInvoice>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_deleted_invoice(response);
        },
        std::move(completion));
}

Task<SubscriptionSchedule>
StripeApiClient::SubscriptionScheduleOperations::retrieve_async(std::string id,
                                                                const RequestOptions& options) {
    return send_and_parse_async<SubscriptionSchedule>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Get, catalog_path("subscription_schedules", id), {}, options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription_schedule(response);
        });
}

void StripeApiClient::SubscriptionScheduleOperations::retrieve(std::string id,
                                                               const RequestOptions& options,
                                                               Completion completion) {
    HttpRequest http;
    try {
        http =
            make_request(HttpMethod::Get, catalog_path("subscription_schedules", id), {}, options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<SubscriptionSchedule>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription_schedule(response);
        },
        std::move(completion));
}

Task<SubscriptionSchedule> StripeApiClient::SubscriptionScheduleOperations::create_async(
    const CreateSubscriptionScheduleRequest& request, const RequestOptions& options) {
    return send_and_parse_async<SubscriptionSchedule>(
        owner.http_client,
        owner.response_parser,
        make_request(
            HttpMethod::Post, "/v1/subscription_schedules", schedule_create_body(request), options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription_schedule(response);
        });
}

void StripeApiClient::SubscriptionScheduleOperations::create(
    const CreateSubscriptionScheduleRequest& request,
    const RequestOptions& options,
    Completion completion) {
    HttpRequest http;
    try {
        http = make_request(
            HttpMethod::Post, "/v1/subscription_schedules", schedule_create_body(request), options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<SubscriptionSchedule>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription_schedule(response);
        },
        std::move(completion));
}

Task<SubscriptionSchedule> StripeApiClient::SubscriptionScheduleOperations::update_async(
    std::string id,
    const UpdateSubscriptionScheduleRequest& request,
    const RequestOptions& options) {
    return send_and_parse_async<SubscriptionSchedule>(
        owner.http_client,
        owner.response_parser,
        make_request(HttpMethod::Post,
                     catalog_path("subscription_schedules", id),
                     schedule_update_body(request),
                     options),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription_schedule(response);
        });
}

void StripeApiClient::SubscriptionScheduleOperations::update(
    std::string id,
    const UpdateSubscriptionScheduleRequest& request,
    const RequestOptions& options,
    Completion completion) {
    HttpRequest http;
    try {
        http = make_request(HttpMethod::Post,
                            catalog_path("subscription_schedules", id),
                            schedule_update_body(request),
                            options);
    } catch (...) {
        completion(std::current_exception());
        return;
    }
    send_and_parse<SubscriptionSchedule>(
        owner.http_client,
        owner.response_parser,
        owner.callback_lifetime,
        std::move(http),
        [](const StripeResponseParser& parser, const HttpResponse& response) {
            return parser.parse_subscription_schedule(response);
        },
        std::move(completion));
}

} // namespace stripekit