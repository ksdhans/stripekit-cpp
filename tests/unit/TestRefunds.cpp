/** TestRefunds.cpp
 *
 * Tests for refund requests, pagination, parsing, and failures.
 */

#include "TestFramework.h"
#include "TestHelpers.h"
#include "StripeKit/StripeApiClient.h"

#include <json/json.h>
#include <sstream>

using namespace stripekit;
using namespace stripekit::testing;

namespace {
const std::string refund_json = R"({
    "object":"refund","id":"re_test","charge":"ch_test","payment_intent":"pi_test",
    "amount":300,"currency":"usd","status":"succeeded","failure_reason":null,
    "metadata":{"operation":"saved & retry"}
})";

/** Build a one-entry Stripe page, or an empty page when entry is empty. */
std::string page(const std::string& entry, bool more = false) {
    return R"({"object":"list","has_more":)" + std::string(more ? "true" : "false") +
           R"(,"data":[)" + entry + "]}";
}

/** Decode the refund fixture for targeted field changes. */
Json::Value fixture() {
    Json::Value value;
    std::istringstream input(refund_json);
    input >> value;
    return value;
}

/** Encode a changed fixture as a successful response. */
HttpResponse response(const Json::Value& value) {
    return { .status_code = 200, .body = Json::writeString(Json::StreamWriterBuilder{}, value) };
}
} // namespace

class RefundsTest : public ::testing::Test {
protected:
    MockHttpClient http;
    StripeApiClient client{ http };
};

TEST_F(RefundsTest, CreateEncodesAmountMetadataAndRetryOptions) {
    http.set_response({ .status_code = 200, .body = refund_json });
    const CreateRefundRequest request{ .charge_id = "ch/a &b",
                                       .amount = 300,
                                       .metadata = { { "operation", "saved & retry" } } };
    const RequestOptions options{ .idempotency_key = "refund-key",
                                  .timeout = std::chrono::seconds(7),
                                  .headers = { { "Stripe-Version", "2025-03-31.basil" } } };
    const auto refund = client.refunds().create_async(request, options).result();
    EXPECT_EQ(refund.id, "re_test");
    EXPECT_EQ(refund.charge_id, "ch_test");
    EXPECT_EQ(refund.payment_intent_id, "pi_test");
    EXPECT_EQ(refund.amount, 300);
    EXPECT_EQ(refund.currency, "usd");
    EXPECT_EQ(refund.status, "succeeded");
    EXPECT_EQ(refund.metadata.at("operation"), "saved & retry");
    EXPECT_TRUE(refund.failure_reason.empty());
    const auto sent = http.get_recorded_requests().at(0);
    EXPECT_EQ(sent.path, "/v1/refunds");
    EXPECT_EQ(sent.method, HttpMethod::Post);
    EXPECT_EQ(sent.body,
              "charge=ch%2Fa%20%26b&amount=300&metadata%5Boperation%5D=saved%20%26%20retry");
    EXPECT_EQ(sent.headers.at("Idempotency-Key"), "refund-key");
    EXPECT_EQ(sent.headers.at("Stripe-Version"), "2025-03-31.basil");
    EXPECT_EQ(sent.options.timeout, options.timeout);
    int calls = 0;
    client.refunds().create(request, options, [&](CallbackResult<Refund> result) {
        ++calls;
        ASSERT_TRUE(std::holds_alternative<Refund>(result));
        EXPECT_EQ(std::get<Refund>(result).id, refund.id);
    });
    EXPECT_EQ(calls, 1);
    client.refunds().create_async({ .charge_id = "ch_test" }).result();
    EXPECT_EQ(http.get_recorded_requests().back().body, "charge=ch_test");
}

TEST_F(RefundsTest, ListsEncodeFiltersAndFollowOnlyExplicitCursors) {
    http.set_response({ .status_code = 200, .body = page(refund_json, true) });
    const RequestOptions options{ .timeout = std::chrono::seconds(4) };
    const auto first =
        client.refunds()
            .list_async({ .charge_id = "ch/a &b", .limit = 1, .starting_after = "re?last" },
                        options)
            .result();
    ASSERT_EQ(first.data.size(), 1U);
    EXPECT_TRUE(first.has_more);
    EXPECT_EQ(http.get_recorded_requests().size(), 1U);
    const auto sent = http.get_recorded_requests().front();
    EXPECT_EQ(sent.path, "/v1/refunds?limit=1&starting_after=re%3Flast&charge=ch%2Fa%20%26b");
    EXPECT_EQ(sent.method, HttpMethod::Get);
    EXPECT_TRUE(sent.body.empty());
    EXPECT_EQ(sent.options.timeout, options.timeout);
    auto next = fixture();
    next["id"] = "re_next";
    http.set_response({ .status_code = 200, .body = page(response(next).body) });
    int calls = 0;
    client.refunds().list(
        { .charge_id = "ch_test", .limit = 100, .starting_after = first.data.back().id },
        options,
        [&](CallbackResult<RefundList> result) {
            ++calls;
            ASSERT_TRUE(std::holds_alternative<RefundList>(result));
            const auto& page = std::get<RefundList>(result);
            EXPECT_FALSE(page.has_more);
            EXPECT_EQ(page.data.front().id, "re_next");
        });
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(http.get_recorded_requests().back().path,
              "/v1/refunds?limit=100&starting_after=re_test&charge=ch_test");
    http.set_response({ .status_code = 200, .body = page("") });
    EXPECT_TRUE(client.refunds().list_async().result().data.empty());
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/refunds?limit=10");
}

TEST_F(RefundsTest, InvalidRequestsFailBeforeSendingAndCallbacksReportOnce) {
    int calls = 0;
    for (const auto& request : { CreateRefundRequest{},
                                 CreateRefundRequest{ .charge_id = "ch", .amount = 0 },
                                 CreateRefundRequest{ .charge_id = "ch", .amount = -1 } }) {
        EXPECT_THROW(client.refunds().create_async(request).result(), ConfigurationException);
        client.refunds().create(request, {}, [&](CallbackResult<Refund> result) {
            ++calls;
            EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
        });
    }
    for (const auto limit : { 0, 101 }) {
        EXPECT_THROW(client.refunds().list_async({ .limit = limit }).result(),
                     ConfigurationException);
        client.refunds().list({ .limit = limit }, {}, [&](CallbackResult<RefundList> result) {
            ++calls;
            EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
        });
    }
    EXPECT_EQ(calls, 5);
    EXPECT_TRUE(http.get_recorded_requests().empty());
}

TEST_F(RefundsTest, ParserPreservesStatusesExpandedReferencesAndFailureReasons) {
    JsonStripeResponseParser parser;
    auto value = fixture();
    value["charge"] = Json::Value(Json::objectValue);
    value["charge"]["id"] = "ch_expanded";
    value["payment_intent"] = Json::Value(Json::objectValue);
    value["payment_intent"]["id"] = "pi_expanded";
    value["failure_reason"] = "lost_or_stolen_card";
    for (const auto* status :
         { "succeeded", "pending", "requires_action", "failed", "canceled", "future" }) {
        value["status"] = status;
        const auto refund = parser.parse_refund(response(value));
        EXPECT_EQ(refund.status, status);
        EXPECT_EQ(refund.charge_id, "ch_expanded");
        EXPECT_EQ(refund.payment_intent_id, "pi_expanded");
        EXPECT_EQ(refund.failure_reason, "lost_or_stolen_card");
    }
    value["payment_intent"] = Json::nullValue;
    EXPECT_TRUE(parser.parse_refund(response(value)).payment_intent_id.empty());
    value["payment_intent"] = "pi_only";
    value["charge"] = Json::nullValue;
    EXPECT_TRUE(parser.parse_refund(response(value)).charge_id.empty());
}

TEST_F(RefundsTest, MalformedRefundsAndPagesAreRejected) {
    JsonStripeResponseParser parser;
    for (const auto* field : { "id", "object", "status", "amount", "currency", "metadata" }) {
        auto value = fixture();
        value.removeMember(field);
        EXPECT_THROW(parser.parse_refund(response(value)), ConfigurationException);
    }
    for (const auto* field : { "amount", "charge", "metadata", "failure_reason" }) {
        auto value = fixture();
        value[field] = -1;
        EXPECT_THROW(parser.parse_refund(response(value)), ConfigurationException);
    }
    auto value = fixture();
    value["charge"] = Json::nullValue;
    value["payment_intent"] = Json::nullValue;
    EXPECT_THROW(parser.parse_refund(response(value)), ConfigurationException);
    value = fixture();
    value["metadata"]["operation"] = false;
    EXPECT_THROW(parser.parse_refund(response(value)), ConfigurationException);
    for (const auto& body : { std::string("{}"),
                              page("", true),
                              page("{}"),
                              page(refund_json + "," + refund_json),
                              std::string(R"({"object":"list","data":[],"has_more":"false"})") }) {
        EXPECT_THROW(parser.parse_refund_list({ .body = body }), ConfigurationException);
    }
}

TEST_F(RefundsTest, ProviderTransportAndParseFailuresReachBothApiForms) {
    int calls = 0;
    const auto create_failure = [&](CallbackResult<Refund> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    };
    const auto list_failure = [&](CallbackResult<RefundList> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    };
    for (const auto status : { 400, 429, 500 }) {
        http.set_response(
            { .status_code = status,
              .headers = { { "request-id", "req_refund" } },
              .body = R"({"error":{"type":"invalid_request_error","message":"Rejected"}})" });
        try {
            client.refunds().create_async({ .charge_id = "ch" }).result();
            FAIL();
        } catch (const StripeApiException& error) {
            EXPECT_EQ(error.payload().http_status, status);
            EXPECT_EQ(error.payload().request_id, "req_refund");
        }
        EXPECT_THROW(client.refunds().list_async().result(), StripeApiException);
        client.refunds().create({ .charge_id = "ch" }, {}, create_failure);
        client.refunds().list({}, {}, list_failure);
    }
    http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    EXPECT_THROW(client.refunds().create_async({ .charge_id = "ch" }).result(), std::runtime_error);
    http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    EXPECT_THROW(client.refunds().list_async().result(), std::runtime_error);
    http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    client.refunds().create({ .charge_id = "ch" }, {}, create_failure);
    http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
    client.refunds().list({}, {}, list_failure);
    http.set_response({ .status_code = 200, .body = "not json" });
    EXPECT_THROW(client.refunds().create_async({ .charge_id = "ch" }).result(),
                 ConfigurationException);
    EXPECT_THROW(client.refunds().list_async().result(), ConfigurationException);
    client.refunds().create({ .charge_id = "ch" }, {}, create_failure);
    client.refunds().list({}, {}, list_failure);
    EXPECT_EQ(calls, 10);
}
