/** TestBillingPortalAndInvoices.cpp
 *
 * Tests for invoice retrieval, billing portal session flows, and billing
 * portal configurations, without contacting Stripe.
 */

#include "TestFramework.h"
#include "TestHelpers.h"

#include "StripeKit/Errors.h"
#include "StripeKit/StripeApiClient.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using namespace stripekit;
using namespace stripekit::testing;

namespace {

using FormFields = std::vector<std::pair<std::string, std::string>>;

/** Decodes one percent-encoded form component. */
std::string form_decode(const std::string& text) {
    std::string result;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '%' && index + 2 < text.size()) {
            result += static_cast<char>(std::stoi(text.substr(index + 1, 2), nullptr, 16));
            index += 2;
        } else {
            result += text[index];
        }
    }
    return result;
}

/** Splits a form-encoded body into decoded name/value pairs, keeping their order. */
FormFields form_fields(const std::string& body) {
    FormFields fields;
    std::size_t start = 0;
    while (start < body.size()) {
        auto end = body.find('&', start);
        if (end == std::string::npos) end = body.size();
        const auto pair = body.substr(start, end - start);
        const auto equals = pair.find('=');
        fields.emplace_back(form_decode(pair.substr(0, equals)),
                            equals == std::string::npos ? ""
                                                        : form_decode(pair.substr(equals + 1)));
        start = end + 1;
    }
    return fields;
}

/** Returns true when the body contains the field with exactly this value. */
bool has_field(const FormFields& fields, const std::string& name, const std::string& value) {
    return std::find(fields.begin(), fields.end(), std::make_pair(name, value)) != fields.end();
}

/** Returns true when the body contains any field whose name starts with the prefix. */
bool has_prefix(const FormFields& fields, const std::string& prefix) {
    return std::any_of(fields.begin(), fields.end(), [&](const auto& field) {
        return field.first.rfind(prefix, 0) == 0;
    });
}

const std::string session_json = R"({
    "id":"bps_123","object":"billing_portal.session","configuration":"bpc_123",
    "customer":"cus_123","url":"https://billing.stripe.com/p/session/test",
    "return_url":"https://example.com/billing","locale":"nl","created":1700000000,
    "livemode":false,"flow":null
})";

const std::string configuration_json = R"({
    "id":"bpc_123","object":"billing_portal.configuration","active":true,"is_default":false,
    "name":"ZitaKit plans","default_return_url":"https://example.com/billing",
    "business_profile":{"headline":"Manage your plan"},"created":1700000000,"livemode":false,
    "metadata":{"app":"zitakit"},
    "features":{
        "customer_update":{"enabled":true,"allowed_updates":["email","address"]},
        "invoice_history":{"enabled":true},
        "payment_method_update":{"enabled":true},
        "subscription_cancel":{"enabled":true,"mode":"at_period_end",
            "proration_behavior":"none","cancellation_reason":{"enabled":false,"options":[]}},
        "subscription_update":{"enabled":true,"default_allowed_updates":["price"],
            "proration_behavior":"create_prorations",
            "products":[{"product":"prod_basic","prices":["price_basic_m","price_basic_y"]},
                        {"product":"prod_pro","prices":["price_pro_m"]}]}
    }
})";

/** A plan-switching feature set listing two products. */
BillingPortalFeatures plan_features() {
    BillingPortalFeatures features;
    features.customer_update = { .enabled = true, .allowed_updates = { "email", "address" } };
    features.subscription_cancel = { .enabled = true,
                                     .mode = "at_period_end",
                                     .proration_behavior = "" };
    features.subscription_update = {
        .enabled = true,
        .default_allowed_updates = { "price" },
        .products = { { .product_id = "prod_basic",
                        .price_ids = { "price_basic_m", "price_basic_y" } },
                      { .product_id = "prod_pro", .price_ids = { "price_pro_m" } } },
        .proration_behavior = "create_prorations"
    };
    return features;
}

/** A confirm flow switching the single subscription item to a new price. */
BillingPortalFlow confirm_flow() {
    return { .type = BillingPortalFlowType::SubscriptionUpdateConfirm,
             .subscription_id = "sub_123",
             .items = { { .item_id = "si_123", .price_id = "price_pro_m", .quantity = 2 } },
             .after_completion = BillingPortalAfterCompletion::Redirect,
             .redirect_url = "https://example.com/billing/done?plan=pro",
             .confirmation_message = "" };
}

/**
 * A response parser that implements only the methods that existed before
 * invoices and portal configurations, like a parser written for an older
 * StripeKit release.
 */
class OlderParser final : public StripeResponseParser {
public:
    Customer parse_customer(const HttpResponse& response) const override {
        return json.parse_customer(response);
    }
    CheckoutSession parse_checkout_session(const HttpResponse& response) const override {
        return json.parse_checkout_session(response);
    }
    Subscription parse_subscription(const HttpResponse& response) const override {
        return json.parse_subscription(response);
    }
    BillingPortalSession parse_billing_portal_session(const HttpResponse& response) const override {
        return json.parse_billing_portal_session(response);
    }
    StripeErrorPayload parse_error(const HttpResponse& response) const override {
        return json.parse_error(response);
    }

private:
    JsonStripeResponseParser json;
};

} // namespace

class BillingPortalAndInvoicesTest : public ::testing::Test {
protected:
    MockHttpClient http;
    StripeApiClient client{ http };
};

TEST_F(BillingPortalAndInvoicesTest, InvoiceRetrieveReadsCurrentApiShape) {
    http.set_response({ .status_code = 200, .body = R"({
        "id":"in_123","object":"invoice","customer":"cus_123","status":"open",
        "currency":"usd","amount_due":2500,"amount_paid":0,"amount_remaining":2500,
        "number":"ABC-0001","hosted_invoice_url":"https://invoice.stripe.com/i/test",
        "invoice_pdf":"https://pay.stripe.com/invoice/test/pdf",
        "billing_reason":"subscription_cycle","attempt_count":1,"created":1700000000,
        "due_date":null,"next_payment_attempt":1700086400,"livemode":false,
        "parent":{"type":"subscription_details",
                  "subscription_details":{"subscription":"sub_123","metadata":{}}}
    })" });
    const auto invoice = client.invoices().retrieve_async("in_123").result();
    const auto sent = http.get_recorded_requests().at(0);
    EXPECT_EQ(sent.method, HttpMethod::Get);
    EXPECT_EQ(sent.path, "/v1/invoices/in_123");
    EXPECT_EQ(invoice.id, "in_123");
    EXPECT_EQ(invoice.customer_id, "cus_123");
    EXPECT_EQ(invoice.subscription_id, "sub_123");
    EXPECT_EQ(invoice.status, "open");
    EXPECT_FALSE(invoice.paid);
    EXPECT_EQ(invoice.amount_due, 2500);
    EXPECT_EQ(invoice.amount_remaining, 2500);
    EXPECT_EQ(invoice.number, "ABC-0001");
    EXPECT_EQ(invoice.hosted_invoice_url, "https://invoice.stripe.com/i/test");
    EXPECT_EQ(invoice.invoice_pdf, "https://pay.stripe.com/invoice/test/pdf");
    EXPECT_EQ(invoice.billing_reason, "subscription_cycle");
    EXPECT_EQ(invoice.attempt_count, 1);
    EXPECT_EQ(invoice.created_unix, 1700000000);
    EXPECT_FALSE(invoice.due_date_unix.has_value());
    ASSERT_TRUE(invoice.next_payment_attempt_unix.has_value());
    EXPECT_EQ(*invoice.next_payment_attempt_unix, 1700086400);
}

TEST(InvoiceParser, ReadsPaidStatusAndNullableParent) {
    const JsonStripeResponseParser parser;
    const auto paid = parser.parse_invoice({ .status_code = 200, .body = R"({
        "id":"in_paid","customer":{"id":"cus_123"},"status":"paid",
        "parent":{"type":"subscription_details","subscription_details":{"subscription":"sub_test"}},
        "amount_due":100,"amount_paid":100,"amount_remaining":0,
        "due_date":1700000000,"livemode":true
    })" });
    EXPECT_EQ(paid.customer_id, "cus_123");
    EXPECT_EQ(paid.subscription_id, "sub_test");
    EXPECT_TRUE(paid.paid);
    EXPECT_TRUE(paid.livemode);
    ASSERT_TRUE(paid.due_date_unix.has_value());
    EXPECT_EQ(*paid.due_date_unix, 1700000000);
    EXPECT_TRUE(paid.hosted_invoice_url.empty());

    const auto current = parser.parse_invoice(
        { .status_code = 200, .body = R"({"id":"in_new","status":"paid","parent":null})" });
    EXPECT_TRUE(current.paid);
    EXPECT_TRUE(current.subscription_id.empty());

    EXPECT_THROW(parser.parse_invoice({ .status_code = 200, .body = R"({"status":"paid"})" }),
                 ConfigurationException);
    EXPECT_THROW(
        parser.parse_invoice({ .status_code = 200, .body = R"({"id":"in_1","due_date":"x"})" }),
        ConfigurationException);
}

TEST_F(BillingPortalAndInvoicesTest, InvoiceCallbacksAndInvalidIdsReportOnce) {
    http.set_response({ .status_code = 200, .body = R"({"id":"in_cb","status":"draft"})" });
    // Checks run after the callbacks, because StripeKit does not let a callback's exception escape.
    std::vector<CallbackResult<Invoice>> results;
    const auto record = [&](CallbackResult<Invoice> result) {
        results.push_back(std::move(result));
    };
    client.invoices().retrieve("in_cb", {}, record);
    client.invoices().retrieve("", {}, record);
    ASSERT_EQ(results.size(), 2U);
    ASSERT_TRUE(std::holds_alternative<Invoice>(results[0]));
    EXPECT_EQ(std::get<Invoice>(results[0]).id, "in_cb");
    EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(results[1]));
    EXPECT_THROW(client.invoices().retrieve_async("").result(), ConfigurationException);
    EXPECT_EQ(http.get_recorded_requests().size(), 1U);
}

TEST_F(BillingPortalAndInvoicesTest, InvoiceApiErrorsBecomeStripeApiExceptions) {
    http.set_response({ .status_code = 404,
                        .headers = { { "request-id", "req_missing" } },
                        .body = R"({"error":{"type":"invalid_request_error",
                                    "code":"resource_missing","message":"No such invoice"}})" });
    try {
        client.invoices().retrieve_async("in_missing").result();
        FAIL() << "Expected StripeApiException";
    } catch (const StripeApiException& error) {
        EXPECT_EQ(error.payload().http_status, 404);
        EXPECT_EQ(error.payload().code, "resource_missing");
        EXPECT_EQ(error.payload().request_id, "req_missing");
    }
}

TEST_F(BillingPortalAndInvoicesTest, PlainPortalSessionSendsOnlyCustomerAndReturnUrl) {
    http.set_response({ .status_code = 200, .body = session_json });
    client.billing_portal()
        .sessions()
        .create_async({ .customer_id = "cus_123", .return_url = "https://example.com/billing" })
        .result();
    const auto fields = form_fields(http.get_recorded_requests().at(0).body);
    EXPECT_EQ(fields.size(), 2U);
    EXPECT_TRUE(has_field(fields, "customer", "cus_123"));
    EXPECT_TRUE(has_field(fields, "return_url", "https://example.com/billing"));
}

TEST_F(BillingPortalAndInvoicesTest, PortalSessionEncodesConfigurationLocaleAndConfirmFlow) {
    http.set_response({ .status_code = 200, .body = session_json });
    const auto session = client.billing_portal()
                             .sessions()
                             .create_async({ .customer_id = "cus_123",
                                             .return_url = "https://example.com/billing",
                                             .configuration_id = "bpc_123",
                                             .locale = "nl",
                                             .flow = confirm_flow() },
                                           { .idempotency_key = "portal-key" })
                             .result();
    EXPECT_EQ(session.configuration_id, "bpc_123");
    EXPECT_EQ(session.locale, "nl");
    const auto sent = http.get_recorded_requests().at(0);
    EXPECT_EQ(sent.path, "/v1/billing_portal/sessions");
    EXPECT_EQ(sent.headers.at("Idempotency-Key"), "portal-key");
    const auto fields = form_fields(sent.body);
    EXPECT_TRUE(has_field(fields, "configuration", "bpc_123"));
    EXPECT_TRUE(has_field(fields, "locale", "nl"));
    EXPECT_TRUE(has_field(fields, "flow_data[type]", "subscription_update_confirm"));
    EXPECT_TRUE(
        has_field(fields, "flow_data[subscription_update_confirm][subscription]", "sub_123"));
    EXPECT_TRUE(
        has_field(fields, "flow_data[subscription_update_confirm][items][0][id]", "si_123"));
    EXPECT_TRUE(has_field(
        fields, "flow_data[subscription_update_confirm][items][0][price]", "price_pro_m"));
    EXPECT_TRUE(
        has_field(fields, "flow_data[subscription_update_confirm][items][0][quantity]", "2"));
    EXPECT_TRUE(has_field(fields, "flow_data[after_completion][type]", "redirect"));
    EXPECT_TRUE(has_field(fields,
                          "flow_data[after_completion][redirect][return_url]",
                          "https://example.com/billing/done?plan=pro"));
}

TEST_F(BillingPortalAndInvoicesTest, PortalSessionEncodesOtherFlowTypes) {
    http.set_response({ .status_code = 200, .body = session_json });
    auto& sessions = client.billing_portal().sessions();
    sessions
        .create_async(
            { .customer_id = "cus_123",
              .flow = BillingPortalFlow{ .type = BillingPortalFlowType::PaymentMethodUpdate } })
        .result();
    sessions
        .create_async(
            { .customer_id = "cus_123",
              .flow = BillingPortalFlow{ .type = BillingPortalFlowType::CustomerUpdate } })
        .result();
    sessions
        .create_async(
            { .customer_id = "cus_123",
              .flow =
                  BillingPortalFlow{ .type = BillingPortalFlowType::SubscriptionCancel,
                                     .subscription_id = "sub_123",
                                     .after_completion =
                                         BillingPortalAfterCompletion::HostedConfirmation,
                                     .confirmation_message = "Sorry to see you go & thanks!" } })
        .result();
    sessions
        .create_async(
            { .customer_id = "cus_123",
              .flow = BillingPortalFlow{ .type = BillingPortalFlowType::SubscriptionUpdate,
                                         .subscription_id = "sub_123",
                                         .after_completion =
                                             BillingPortalAfterCompletion::PortalHomepage } })
        .result();
    sessions
        .create_async(
            { .customer_id = "cus_123",
              .flow = BillingPortalFlow{ .type = BillingPortalFlowType::SubscriptionPause,
                                         .subscription_id = "sub_123" } })
        .result();
    const auto requests = http.get_recorded_requests();
    ASSERT_EQ(requests.size(), 5U);

    const auto payment = form_fields(requests[0].body);
    EXPECT_TRUE(has_field(payment, "flow_data[type]", "payment_method_update"));
    EXPECT_FALSE(has_prefix(payment, "flow_data[payment_method_update]"));
    EXPECT_FALSE(has_prefix(payment, "flow_data[after_completion]"));
    EXPECT_FALSE(has_prefix(payment, "return_url"));

    const auto customer = form_fields(requests[1].body);
    EXPECT_TRUE(has_field(customer, "flow_data[type]", "customer_update"));
    EXPECT_FALSE(has_prefix(customer, "flow_data[customer_update]"));
    EXPECT_FALSE(has_prefix(customer, "flow_data[after_completion]"));

    const auto cancel = form_fields(requests[2].body);
    EXPECT_TRUE(has_field(cancel, "flow_data[type]", "subscription_cancel"));
    EXPECT_TRUE(has_field(cancel, "flow_data[subscription_cancel][subscription]", "sub_123"));
    EXPECT_TRUE(has_field(cancel, "flow_data[after_completion][type]", "hosted_confirmation"));
    EXPECT_TRUE(has_field(cancel,
                          "flow_data[after_completion][hosted_confirmation][custom_message]",
                          "Sorry to see you go & thanks!"));

    const auto update = form_fields(requests[3].body);
    EXPECT_TRUE(has_field(update, "flow_data[type]", "subscription_update"));
    EXPECT_TRUE(has_field(update, "flow_data[subscription_update][subscription]", "sub_123"));
    EXPECT_TRUE(has_field(update, "flow_data[after_completion][type]", "portal_homepage"));

    const auto pause = form_fields(requests[4].body);
    EXPECT_TRUE(has_field(pause, "flow_data[type]", "subscription_pause"));
    EXPECT_TRUE(has_field(pause, "flow_data[subscription_pause][subscription]", "sub_123"));
}

TEST_F(BillingPortalAndInvoicesTest, InvalidPortalFlowsFailWithoutNetworkCalls) {
    auto& sessions = client.billing_portal().sessions();
    const auto rejects = [&](CreateBillingPortalSessionRequest request) {
        EXPECT_THROW(sessions.create_async(request).result(), ConfigurationException);
        bool reported = false;
        sessions.create(request, {}, [&](CallbackResult<BillingPortalSession> result) {
            reported = std::holds_alternative<std::exception_ptr>(result);
        });
        EXPECT_TRUE(reported);
    };
    const auto with_flow = [](BillingPortalFlow flow) {
        return CreateBillingPortalSessionRequest{ .customer_id = "cus_123", .flow = flow };
    };

    rejects({ .return_url = "https://example.com" });
    rejects(with_flow({ .type = BillingPortalFlowType::SubscriptionCancel }));
    rejects(with_flow(
        { .type = BillingPortalFlowType::PaymentMethodUpdate, .subscription_id = "sub_123" }));
    rejects(with_flow(
        { .type = BillingPortalFlowType::CustomerUpdate, .subscription_id = "sub_123" }));
    rejects(with_flow({ .type = BillingPortalFlowType::SubscriptionPause }));
    auto flow = confirm_flow();
    flow.items.clear();
    rejects(with_flow(flow));
    flow = confirm_flow();
    flow.items.push_back(flow.items.front());
    rejects(with_flow(flow));
    flow = confirm_flow();
    flow.items.front().item_id.clear();
    rejects(with_flow(flow));
    flow = confirm_flow();
    flow.items.front().price_id.clear();
    flow.items.front().quantity.reset();
    rejects(with_flow(flow));
    flow = confirm_flow();
    flow.items.front().quantity = 0;
    rejects(with_flow(flow));
    flow = confirm_flow();
    flow.redirect_url.clear();
    rejects(with_flow(flow));
    flow = confirm_flow();
    flow.after_completion.reset();
    rejects(with_flow(flow));
    rejects(with_flow({ .type = BillingPortalFlowType::SubscriptionUpdate,
                        .subscription_id = "sub_123",
                        .items = { { .item_id = "si_123", .price_id = "price_1" } } }));
    rejects(with_flow({ .type = BillingPortalFlowType::SubscriptionCancel,
                        .subscription_id = "sub_123",
                        .after_completion = BillingPortalAfterCompletion::PortalHomepage,
                        .confirmation_message = "Not shown" }));
    rejects(with_flow({ .type = BillingPortalFlowType::SubscriptionCancel,
                        .subscription_id = "sub_123",
                        .after_completion = BillingPortalAfterCompletion::HostedConfirmation,
                        .confirmation_message = std::string(501, 'x') }));
    EXPECT_TRUE(http.get_recorded_requests().empty());
}

TEST_F(BillingPortalAndInvoicesTest, ConfigurationCreateSendsEveryFeatureExplicitly) {
    http.set_response({ .status_code = 200, .body = configuration_json });
    const auto created = client.billing_portal()
                             .configurations()
                             .create_async({ .features = {},
                                             .name = "Minimal",
                                             .headline = "",
                                             .default_return_url = "",
                                             .metadata = {} })
                             .result();
    EXPECT_EQ(created.id, "bpc_123");
    const auto sent = http.get_recorded_requests().at(0);
    EXPECT_EQ(sent.path, "/v1/billing_portal/configurations");
    EXPECT_EQ(sent.method, HttpMethod::Post);
    const auto fields = form_fields(sent.body);
    EXPECT_TRUE(has_field(fields, "expand[]", "features.subscription_update.products"));
    EXPECT_TRUE(has_field(fields, "features[invoice_history][enabled]", "true"));
    EXPECT_TRUE(has_field(fields, "features[payment_method_update][enabled]", "true"));
    EXPECT_TRUE(has_field(fields, "features[customer_update][enabled]", "false"));
    EXPECT_TRUE(has_field(fields, "features[subscription_cancel][enabled]", "false"));
    EXPECT_TRUE(has_field(fields, "features[subscription_update][enabled]", "false"));
    EXPECT_FALSE(has_prefix(fields, "features[subscription_update][products]"));
    EXPECT_FALSE(has_prefix(fields, "features[customer_update][allowed_updates]"));
    EXPECT_TRUE(has_field(fields, "name", "Minimal"));
    EXPECT_FALSE(has_prefix(fields, "business_profile"));
    EXPECT_FALSE(has_prefix(fields, "default_return_url"));
}

TEST_F(BillingPortalAndInvoicesTest, ConfigurationCreateEncodesPlanSwitching) {
    http.set_response({ .status_code = 200, .body = configuration_json });
    client.billing_portal()
        .configurations()
        .create_async({ .features = plan_features(),
                        .name = "ZitaKit plans",
                        .headline = "Manage your plan",
                        .default_return_url = "https://example.com/billing",
                        .metadata = { { "app", "zitakit" } } })
        .result();
    const auto fields = form_fields(http.get_recorded_requests().at(0).body);
    EXPECT_TRUE(has_field(fields, "features[customer_update][enabled]", "true"));
    EXPECT_TRUE(has_field(fields, "features[customer_update][allowed_updates][]", "email"));
    EXPECT_TRUE(has_field(fields, "features[customer_update][allowed_updates][]", "address"));
    EXPECT_TRUE(has_field(fields, "features[subscription_cancel][mode]", "at_period_end"));
    EXPECT_FALSE(has_prefix(fields, "features[subscription_cancel][proration_behavior]"));
    EXPECT_TRUE(
        has_field(fields, "features[subscription_update][default_allowed_updates][]", "price"));
    EXPECT_TRUE(
        has_field(fields, "features[subscription_update][products][0][product]", "prod_basic"));
    EXPECT_TRUE(
        has_field(fields, "features[subscription_update][products][0][prices][]", "price_basic_m"));
    EXPECT_TRUE(
        has_field(fields, "features[subscription_update][products][0][prices][]", "price_basic_y"));
    EXPECT_TRUE(
        has_field(fields, "features[subscription_update][products][1][product]", "prod_pro"));
    EXPECT_TRUE(
        has_field(fields, "features[subscription_update][products][1][prices][]", "price_pro_m"));
    EXPECT_TRUE(has_field(
        fields, "features[subscription_update][proration_behavior]", "create_prorations"));
    EXPECT_TRUE(has_field(fields, "business_profile[headline]", "Manage your plan"));
    EXPECT_TRUE(has_field(fields, "default_return_url", "https://example.com/billing"));
    EXPECT_TRUE(has_field(fields, "metadata[app]", "zitakit"));
}

TEST_F(BillingPortalAndInvoicesTest, ConfigurationUpdateSendsOnlyChangedFields) {
    http.set_response({ .status_code = 200, .body = configuration_json });
    auto& configurations = client.billing_portal().configurations();
    configurations.update_async("bpc_123", { .active = false }).result();
    configurations
        .update_async("bpc_123", { .headline = std::string{}, .default_return_url = std::string{} })
        .result();
    auto features = plan_features();
    features.customer_update.allowed_updates.clear();
    configurations.update_async("bpc_123", { .features = features }).result();

    const auto requests = http.get_recorded_requests();
    ASSERT_EQ(requests.size(), 3U);
    EXPECT_EQ(requests[0].path, "/v1/billing_portal/configurations/bpc_123");
    const auto deactivate = form_fields(requests[0].body);
    EXPECT_EQ(deactivate.size(), 2U);
    EXPECT_TRUE(has_field(deactivate, "expand[]", "features.subscription_update.products"));
    EXPECT_TRUE(has_field(deactivate, "active", "false"));

    const auto cleared = form_fields(requests[1].body);
    EXPECT_EQ(cleared.size(), 3U);
    EXPECT_TRUE(has_field(cleared, "business_profile[headline]", ""));
    EXPECT_TRUE(has_field(cleared, "default_return_url", ""));

    const auto replaced = form_fields(requests[2].body);
    EXPECT_TRUE(has_field(replaced, "features[customer_update][allowed_updates]", ""));
    EXPECT_TRUE(
        has_field(replaced, "features[subscription_update][products][1][product]", "prod_pro"));
    EXPECT_FALSE(has_prefix(replaced, "active"));
}

TEST_F(BillingPortalAndInvoicesTest, InvalidConfigurationsFailWithoutNetworkCalls) {
    auto& configurations = client.billing_portal().configurations();
    const auto rejects = [&](BillingPortalFeatures features) {
        EXPECT_THROW(configurations.create_async({ .features = features }).result(),
                     ConfigurationException);
        EXPECT_THROW(configurations.update_async("bpc_123", { .features = features }).result(),
                     ConfigurationException);
    };
    auto features = plan_features();
    features.customer_update.allowed_updates = { "password" };
    rejects(features);
    features = plan_features();
    features.subscription_cancel.mode = "whenever";
    rejects(features);
    features = plan_features();
    features.subscription_cancel.proration_behavior = "always_invoice";
    rejects(features);
    features = plan_features();
    features.subscription_update.proration_behavior = "sometimes";
    rejects(features);
    features = plan_features();
    features.subscription_update.default_allowed_updates = { "plan" };
    rejects(features);
    features = plan_features();
    features.subscription_update.products.resize(11,
                                                 { .product_id = "prod", .price_ids = { "p" } });
    rejects(features);
    features = plan_features();
    features.subscription_update.products.front().price_ids.clear();
    rejects(features);
    features = plan_features();
    features.subscription_update.products.front().product_id.clear();
    rejects(features);
    features = plan_features();
    features.subscription_update.products.clear();
    rejects(features);
    features = plan_features();
    features.subscription_update.default_allowed_updates.clear();
    rejects(features);
    features = plan_features();
    features.payment_method_update_enabled = false;
    rejects(features);

    EXPECT_THROW(configurations.create_async({ .name = std::string(257, 'n') }).result(),
                 ConfigurationException);
    EXPECT_THROW(configurations.retrieve_async("").result(), ConfigurationException);
    EXPECT_THROW(configurations.update_async("", { .active = false }).result(),
                 ConfigurationException);

    int failures = 0;
    const auto failed = [&](CallbackResult<BillingPortalConfiguration> result) {
        failures += std::holds_alternative<std::exception_ptr>(result) ? 1 : 100;
    };
    configurations.create({ .name = std::string(257, 'n') }, {}, failed);
    configurations.retrieve("", {}, failed);
    configurations.update("", {}, {}, failed);
    EXPECT_EQ(failures, 3);
    EXPECT_TRUE(http.get_recorded_requests().empty());
}

TEST_F(BillingPortalAndInvoicesTest, ConfigurationCallbacksReturnParsedResults) {
    http.set_response({ .status_code = 200, .body = configuration_json });
    auto& configurations = client.billing_portal().configurations();
    std::vector<CallbackResult<BillingPortalConfiguration>> results;
    const auto parsed = [&](CallbackResult<BillingPortalConfiguration> result) {
        results.push_back(std::move(result));
    };
    configurations.create({}, {}, parsed);
    configurations.retrieve("bpc_123", {}, parsed);
    configurations.update("bpc_123", { .name = "Renamed" }, {}, parsed);
    ASSERT_EQ(results.size(), 3U);
    for (const auto& result : results) {
        ASSERT_TRUE(std::holds_alternative<BillingPortalConfiguration>(result));
        EXPECT_EQ(std::get<BillingPortalConfiguration>(result).id, "bpc_123");
    }
    const auto requests = http.get_recorded_requests();
    ASSERT_EQ(requests.size(), 3U);
    EXPECT_EQ(requests[1].method, HttpMethod::Get);
    EXPECT_EQ(requests[1].path,
              std::string("/v1/billing_portal/configurations/bpc_123?") +
                  "expand%5B%5D=features.subscription_update.products");
    EXPECT_TRUE(has_field(form_fields(requests[2].body), "name", "Renamed"));
}

TEST(BillingPortalConfigurationParser, ReadsEveryFeature) {
    const auto configuration = JsonStripeResponseParser{}.parse_billing_portal_configuration(
        { .status_code = 200, .body = configuration_json });
    EXPECT_EQ(configuration.id, "bpc_123");
    EXPECT_TRUE(configuration.active);
    EXPECT_FALSE(configuration.is_default);
    EXPECT_EQ(configuration.name, "ZitaKit plans");
    EXPECT_EQ(configuration.headline, "Manage your plan");
    EXPECT_EQ(configuration.default_return_url, "https://example.com/billing");
    EXPECT_EQ(configuration.created_unix, 1700000000);
    EXPECT_EQ(configuration.metadata.at("app"), "zitakit");
    const auto& features = configuration.features;
    EXPECT_TRUE(features.customer_update.enabled);
    EXPECT_EQ(features.customer_update.allowed_updates,
              (std::vector<std::string>{ "email", "address" }));
    EXPECT_TRUE(features.invoice_history_enabled);
    EXPECT_TRUE(features.payment_method_update_enabled);
    EXPECT_TRUE(features.subscription_cancel.enabled);
    EXPECT_EQ(features.subscription_cancel.mode, "at_period_end");
    EXPECT_EQ(features.subscription_cancel.proration_behavior, "none");
    EXPECT_TRUE(features.subscription_update.enabled);
    EXPECT_EQ(features.subscription_update.proration_behavior, "create_prorations");
    ASSERT_EQ(features.subscription_update.products.size(), 2U);
    EXPECT_EQ(features.subscription_update.products[0].product_id, "prod_basic");
    EXPECT_EQ(features.subscription_update.products[0].price_ids,
              (std::vector<std::string>{ "price_basic_m", "price_basic_y" }));
}

TEST(BillingPortalConfigurationParser, ReportsMissingFeaturesAsOffAndAcceptsListProducts) {
    const JsonStripeResponseParser parser;
    const auto sparse = parser.parse_billing_portal_configuration({ .status_code = 200, .body = R"({
        "id":"bpc_sparse","active":false,"business_profile":null,
        "features":{"subscription_update":{"enabled":true,"default_allowed_updates":["price"],
            "products":{"object":"list","has_more":false,
                        "data":[{"product":"prod_a","prices":["price_a"]}]}}}
    })" });
    EXPECT_FALSE(sparse.active);
    EXPECT_TRUE(sparse.headline.empty());
    EXPECT_FALSE(sparse.features.invoice_history_enabled);
    EXPECT_FALSE(sparse.features.payment_method_update_enabled);
    EXPECT_FALSE(sparse.features.customer_update.enabled);
    ASSERT_EQ(sparse.features.subscription_update.products.size(), 1U);
    EXPECT_EQ(sparse.features.subscription_update.products[0].price_ids.at(0), "price_a");

    EXPECT_THROW(parser.parse_billing_portal_configuration(
                     { .status_code = 200, .body = R"({"id":"bpc_1"})" }),
                 ConfigurationException);
    EXPECT_THROW(parser.parse_billing_portal_configuration(
                     { .status_code = 200, .body = R"({"id":"bpc_1","features":{"customer_update":
                                  {"allowed_updates":"email"}}})" }),
                 ConfigurationException);
    EXPECT_THROW(
        parser.parse_billing_portal_configuration(
            { .status_code = 200, .body = R"({"id":"bpc_1","features":{"subscription_update":
                                  {"products":[{"prices":["price_a"]}]}}})" }),
        ConfigurationException);
}

TEST(OlderResponseParser, StillDecodesInvoicesAndConfigurations) {
    MockHttpClient http;
    const OlderParser parser;
    StripeApiClient client(http, parser);
    http.set_response({ .status_code = 200, .body = R"({"id":"in_123","status":"paid"})" });
    EXPECT_TRUE(client.invoices().retrieve_async("in_123").result().paid);
    http.set_response({ .status_code = 200, .body = configuration_json });
    EXPECT_EQ(client.billing_portal().configurations().retrieve_async("bpc_123").result().id,
              "bpc_123");
}
