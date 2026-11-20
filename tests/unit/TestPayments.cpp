/** TestPayments.cpp
 *
 * Tests for charge reads and invoice payment allocations without contacting Stripe.
 */

#include "TestFramework.h"
#include "TestHelpers.h"
#include "StripeKit/StripeApiClient.h"

#include <json/json.h>
#include <variant>

using namespace stripekit;
using namespace stripekit::testing;

namespace {

const std::string charge_json = R"({
    "object":"charge","id":"ch_test","customer":"cus_test","payment_intent":"pi_test",
    "amount":1500,"amount_refunded":200,"currency":"usd",
    "status":"succeeded","paid":true,"disputed":false,"livemode":false
})";
const std::string allocation_json = R"({
    "object":"invoice_payment","id":"inpay_test","invoice":"in_test",
    "payment":{"type":"payment_intent","payment_intent":"pi_test"},
    "amount_requested":1500,"amount_paid":1500,"currency":"usd",
    "status":"paid","is_default":true,"livemode":false
})";

/** Wrap a single object in a Stripe list page. */
std::string page(const std::string& entry, bool more = false) {
    return R"({"object":"list","has_more":)" + std::string(more ? "true" : "false") +
           R"(,"data":[)" + entry + "]}";
}

/** Encode a changed fixture for nullable, expanded, and malformed response tests. */
std::string json_body(const Json::Value& value) {
    return Json::writeString(Json::StreamWriterBuilder{}, value);
}

/** Decode a fixture so tests can change individual fields. */
Json::Value fixture(const std::string& text) {
    Json::Value result;
    std::istringstream input(text);
    input >> result;
    return result;
}

} // namespace

class PaymentsTest : public ::testing::Test {
protected:
    MockHttpClient http;
    StripeApiClient client{ http };
};

TEST_F(PaymentsTest, RetrieveChargeEncodesIdAndPreservesOptionsAndRelationships) {
    http.set_response({ .status_code = 200, .body = charge_json });
    const RequestOptions options{ .timeout = std::chrono::seconds(7),
                                  .headers = { { "Stripe-Version", "2026-10-28.endive" } } };
    const auto charge = client.charges().retrieve_async("ch/a?b", options).result();
    EXPECT_EQ(charge.id, "ch_test");
    EXPECT_EQ(charge.customer_id, "cus_test");
    EXPECT_EQ(charge.payment_intent_id, "pi_test");
    EXPECT_EQ(charge.amount, 1500);
    EXPECT_EQ(charge.amount_refunded, 200);
    EXPECT_TRUE(charge.paid);
    EXPECT_FALSE(charge.disputed);
    EXPECT_FALSE(charge.livemode);
    const auto sent = http.get_recorded_requests().at(0);
    EXPECT_EQ(sent.method, HttpMethod::Get);
    EXPECT_EQ(sent.path, "/v1/charges/ch%2Fa%3Fb");
    EXPECT_TRUE(sent.body.empty());
    EXPECT_EQ(sent.options.timeout, options.timeout);
    EXPECT_EQ(sent.headers.at("Stripe-Version"), "2026-10-28.endive");
    int calls = 0;
    client.charges().retrieve("ch_test", options, [&](CallbackResult<Charge> result) {
        ++calls;
        ASSERT_TRUE(std::holds_alternative<Charge>(result));
        EXPECT_EQ(std::get<Charge>(result).id, "ch_test");
    });
    EXPECT_EQ(calls, 1);
}

TEST_F(PaymentsTest, ListsEncodeFiltersCursorsAndDoNotAutomaticallyTraverse) {
    http.set_response({ .status_code = 200, .body = page(charge_json, true) });
    const RequestOptions options{ .timeout = std::chrono::seconds(9),
                                  .headers = { { "Stripe-Version", "2026-10-28.endive" } } };
    const auto charges =
        client.charges()
            .list_async({ .customer_id = "cus &one", .limit = 100, .starting_after = "ch/a" },
                        options)
            .result();
    ASSERT_EQ(charges.data.size(), 1U);
    EXPECT_TRUE(charges.has_more);
    EXPECT_EQ(http.get_recorded_requests().size(), 1U);
    EXPECT_EQ(http.get_recorded_requests()[0].path,
              "/v1/charges?limit=100&starting_after=ch%2Fa&customer=cus%20%26one");
    EXPECT_EQ(http.get_recorded_requests()[0].options.timeout, options.timeout);

    http.set_response({ .status_code = 200, .body = page(allocation_json, true) });
    const auto allocations =
        client.invoice_payments()
            .list_async(
                { .payment_intent_id = "pi/a &b", .limit = 1, .starting_after = "inpay?one" },
                options)
            .result();
    ASSERT_EQ(allocations.data.size(), 1U);
    EXPECT_TRUE(allocations.has_more);
    EXPECT_EQ(http.get_recorded_requests().size(), 2U);
    const auto sent = http.get_recorded_requests().back();
    EXPECT_EQ(sent.path,
              "/v1/"
              "invoice_payments?limit=1&starting_after=inpay%3Fone&payment%5Btype%5D=payment_"
              "intent&payment%5Bpayment_intent%5D=pi%2Fa%20%26b");
    EXPECT_EQ(sent.method, HttpMethod::Get);
    EXPECT_TRUE(sent.body.empty());
    EXPECT_EQ(sent.options.timeout, options.timeout);
    EXPECT_EQ(sent.headers.at("Stripe-Version"), "2026-10-28.endive");
    const auto& allocation = allocations.data.front();
    EXPECT_EQ(allocation.invoice_id, "in_test");
    EXPECT_EQ(allocation.payment_type, "payment_intent");
    EXPECT_EQ(allocation.payment_intent_id, "pi_test");
    EXPECT_EQ(allocation.amount_requested, 1500);
    EXPECT_EQ(allocation.amount_paid.value(), 1500);
    EXPECT_TRUE(allocation.is_default);
}

TEST_F(PaymentsTest, CallbackListsReturnDataAndDefaultFiltersAreOmitted) {
    int calls = 0;
    http.set_response({ .status_code = 200, .body = page(charge_json) });
    client.charges().list({}, {}, [&](CallbackResult<ChargeList> result) {
        ++calls;
        ASSERT_TRUE(std::holds_alternative<ChargeList>(result));
        EXPECT_EQ(std::get<ChargeList>(result).data.front().id, "ch_test");
    });
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/charges?limit=10");
    http.set_response({ .status_code = 200, .body = page(allocation_json) });
    client.invoice_payments().list({}, {}, [&](CallbackResult<InvoicePaymentList> result) {
        ++calls;
        ASSERT_TRUE(std::holds_alternative<InvoicePaymentList>(result));
        EXPECT_EQ(std::get<InvoicePaymentList>(result).data.front().id, "inpay_test");
    });
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/invoice_payments?limit=10");
    EXPECT_EQ(calls, 2);
    http.set_response({ .status_code = 200, .body = page("") });
    EXPECT_TRUE(client.charges().list_async().result().data.empty());
    EXPECT_FALSE(client.invoice_payments().list_async().result().has_more);
}

TEST_F(PaymentsTest, InvalidInputsFailWithoutSendingRequests) {
    EXPECT_THROW(client.charges().retrieve_async("").result(), ConfigurationException);
    int calls = 0;
    client.charges().retrieve("", {}, [&](CallbackResult<Charge> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    });
    for (const auto limit : { 0, 101 }) {
        EXPECT_THROW(client.charges().list_async({ .limit = limit }).result(),
                     ConfigurationException);
        EXPECT_THROW(client.invoice_payments().list_async({ .limit = limit }).result(),
                     ConfigurationException);
        client.charges().list({ .limit = limit }, {}, [&](CallbackResult<ChargeList> result) {
            ++calls;
            EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
        });
        client.invoice_payments().list(
            { .limit = limit }, {}, [&](CallbackResult<InvoicePaymentList> result) {
                ++calls;
                EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
            });
    }
    EXPECT_EQ(calls, 5);
    EXPECT_TRUE(http.get_recorded_requests().empty());
}

TEST_F(PaymentsTest, NullableAndExpandedReferencesAndFuturePaymentTypesArePreserved) {
    JsonStripeResponseParser parser;
    auto charge = fixture(charge_json);
    charge["customer"] = Json::Value(Json::objectValue);
    charge["customer"]["id"] = "cus_expanded";
    charge["payment_intent"] = Json::Value(Json::objectValue);
    charge["payment_intent"]["id"] = "pi_expanded";
    const auto parsed = parser.parse_charge({ .body = json_body(charge) });
    EXPECT_EQ(parsed.customer_id, "cus_expanded");
    EXPECT_EQ(parsed.payment_intent_id, "pi_expanded");
    charge["customer"] = Json::nullValue;
    charge["payment_intent"] = Json::nullValue;
    EXPECT_TRUE(parser.parse_charge({ .body = json_body(charge) }).customer_id.empty());

    auto allocation = fixture(allocation_json);
    allocation["invoice"] = Json::Value(Json::objectValue);
    allocation["invoice"]["id"] = "in_expanded";
    allocation["payment"]["payment_intent"] = Json::Value(Json::objectValue);
    allocation["payment"]["payment_intent"]["id"] = "pi_expanded";
    allocation["amount_paid"] = Json::nullValue;
    allocation["status"] = "open";
    const auto open = parser.parse_invoice_payment({ .body = json_body(allocation) });
    EXPECT_FALSE(open.amount_paid.has_value());
    EXPECT_EQ(open.invoice_id, "in_expanded");
    EXPECT_EQ(open.payment_intent_id, "pi_expanded");
    for (const auto* type : { "charge", "payment_record", "future_payment" }) {
        allocation["payment"] = Json::Value(Json::objectValue);
        allocation["payment"]["type"] = type;
        if (std::string(type) != "future_payment") allocation["payment"][type] = "reference_test";
        const auto other = parser.parse_invoice_payment({ .body = json_body(allocation) });
        EXPECT_EQ(other.payment_type, type);
        EXPECT_TRUE(other.payment_intent_id.empty());
        if (std::string(type) == "charge") EXPECT_EQ(other.charge_id, "reference_test");
        if (std::string(type) == "payment_record")
            EXPECT_EQ(other.payment_record_id, "reference_test");
    }
}

TEST_F(PaymentsTest, MalformedObjectsAndPagesAreRejectedRatherThanDefaulted) {
    JsonStripeResponseParser parser;
    for (const auto* field : { "id",
                               "status",
                               "amount",
                               "amount_refunded",
                               "currency",
                               "paid",
                               "disputed",
                               "livemode",
                               "object" }) {
        auto value = fixture(charge_json);
        value.removeMember(field);
        EXPECT_THROW(parser.parse_charge({ .body = json_body(value) }), ConfigurationException);
    }
    for (const auto* field : { "id",
                               "invoice",
                               "payment",
                               "status",
                               "amount_requested",
                               "currency",
                               "is_default",
                               "livemode",
                               "object" }) {
        auto value = fixture(allocation_json);
        value.removeMember(field);
        EXPECT_THROW(parser.parse_invoice_payment({ .body = json_body(value) }),
                     ConfigurationException);
    }
    auto value = fixture(allocation_json);
    value["amount_paid"] = -1;
    EXPECT_THROW(parser.parse_invoice_payment({ .body = json_body(value) }),
                 ConfigurationException);
    value = fixture(allocation_json);
    value["payment"].removeMember("payment_intent");
    EXPECT_THROW(parser.parse_invoice_payment({ .body = json_body(value) }),
                 ConfigurationException);
    auto charge = fixture(charge_json);
    charge["customer"] = 42;
    EXPECT_THROW(parser.parse_charge({ .body = json_body(charge) }), ConfigurationException);
    for (const auto* body : { "{}",
                              R"({"object":"list","data":[],"has_more":true})",
                              R"({"object":"list","data":[],"has_more":"false"})",
                              R"({"object":"list","data":{},"has_more":false})" }) {
        EXPECT_THROW(parser.parse_charge_list({ .body = body }), ConfigurationException);
        EXPECT_THROW(parser.parse_invoice_payment_list({ .body = body }), ConfigurationException);
    }
    EXPECT_THROW(parser.parse_charge_list({ .body = page(allocation_json) }),
                 ConfigurationException);
    EXPECT_THROW(parser.parse_invoice_payment_list({ .body = page(charge_json) }),
                 ConfigurationException);
    EXPECT_THROW(parser.parse_charge_list({ .body = page(charge_json + "," + charge_json) }),
                 ConfigurationException);
    EXPECT_THROW(parser.parse_invoice_payment_list(
                     { .body = page(allocation_json + "," + allocation_json) }),
                 ConfigurationException);
}

TEST_F(PaymentsTest, ProviderTransportAndParsingErrorsReachBothApiForms) {
    int calls = 0;
    const auto charge_failure = [&](CallbackResult<Charge> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    };
    const auto charges_failure = [&](CallbackResult<ChargeList> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    };
    const auto allocations_failure = [&](CallbackResult<InvoicePaymentList> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    };
    for (const auto status : { 404, 429, 500 }) {
        http.set_response(
            { .status_code = status,
              .headers = { { "request-id", "req_payment" } },
              .body = R"({"error":{"type":"invalid_request_error","message":"Rejected"}})" });
        try {
            client.charges().retrieve_async("ch_test").result();
            FAIL();
        } catch (const StripeApiException& error) {
            EXPECT_EQ(error.payload().http_status, status);
            EXPECT_EQ(error.payload().request_id, "req_payment");
        }
        EXPECT_THROW(client.charges().list_async().result(), StripeApiException);
        EXPECT_THROW(client.invoice_payments().list_async().result(), StripeApiException);
        client.charges().retrieve("ch_test", {}, charge_failure);
        client.charges().list({}, {}, charges_failure);
        client.invoice_payments().list({}, {}, allocations_failure);
    }
    http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    EXPECT_THROW(client.charges().retrieve_async("ch_test").result(), std::runtime_error);
    http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    EXPECT_THROW(client.charges().list_async().result(), std::runtime_error);
    http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    EXPECT_THROW(client.invoice_payments().list_async().result(), std::runtime_error);
    http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    client.charges().retrieve("ch_test", {}, charge_failure);
    http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    client.charges().list({}, {}, charges_failure);
    http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    client.invoice_payments().list({}, {}, allocations_failure);
    http.set_response({ .status_code = 200, .body = "not json" });
    EXPECT_THROW(client.charges().retrieve_async("ch_test").result(), ConfigurationException);
    EXPECT_THROW(client.charges().list_async().result(), ConfigurationException);
    EXPECT_THROW(client.invoice_payments().list_async().result(), ConfigurationException);
    client.charges().retrieve("ch_test", {}, charge_failure);
    client.charges().list({}, {}, charges_failure);
    client.invoice_payments().list({}, {}, allocations_failure);
    EXPECT_EQ(calls, 15);
}
