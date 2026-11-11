/** TestCollectionAndInvoices.cpp
 *
 * Tests for subscription collection controls and invoice management.
 */
#include "TestFramework.h"
#include "TestHelpers.h"
#include "StripeKit/StripeApiClient.h"

using namespace stripekit;
using namespace stripekit::testing;

namespace {
const std::string subscription_json =
    R"({"id":"sub_test","customer":"cus_test","status":"active","cancel_at_period_end":true,"pause_collection":{"behavior":"void","resumes_at":2000000000}})";
const std::string invoice_json =
    R"({"object":"invoice","id":"in_test","customer":"cus_test","subscription":"sub_test","status":"void","auto_advance":false})";
const std::string deleted_json = R"({"object":"invoice","id":"in_test","deleted":true})";
} // namespace

class CollectionAndInvoicesTest : public ::testing::Test {
protected:
    MockHttpClient http;
    StripeApiClient client{ http };
};

TEST_F(CollectionAndInvoicesTest, SubscriptionControlsEncodeBodiesAndPreserveOptions) {
    http.set_response({ .status_code = 200, .body = subscription_json });
    const RequestOptions options{ .idempotency_key = "saved-control",
                                  .timeout = std::chrono::seconds(8),
                                  .headers = { { "Stripe-Version", "2025-03-31.basil" } } };
    const auto subscription =
        client.subscriptions().set_cancel_at_period_end_async("sub/a?", true, options).result();
    EXPECT_TRUE(subscription.cancel_at_period_end);
    ASSERT_TRUE(subscription.pause_collection.has_value());
    EXPECT_EQ(subscription.pause_collection->behavior, "void");
    EXPECT_EQ(subscription.pause_collection->resumes_at_unix.value(), 2000000000);
    EXPECT_EQ(http.get_recorded_requests().back().body, "cancel_at_period_end=true");
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/subscriptions/sub%2Fa%3F");
    client.subscriptions().set_cancel_at_period_end_async("sub_test", false, options).result();
    EXPECT_EQ(http.get_recorded_requests().back().body, "cancel_at_period_end=false");
    for (const auto& [behavior, name] :
         std::vector<std::pair<CollectionPauseBehavior, std::string>>{
             { CollectionPauseBehavior::Void, "void" },
             { CollectionPauseBehavior::KeepAsDraft, "keep_as_draft" },
             { CollectionPauseBehavior::MarkUncollectible, "mark_uncollectible" } }) {
        client.subscriptions()
            .pause_collection_async("sub_test", { behavior, 2000000000 }, options)
            .result();
        EXPECT_EQ(http.get_recorded_requests().back().body,
                  "pause_collection%5Bbehavior%5D=" + name +
                      "&pause_collection%5Bresumes_at%5D=2000000000");
    }
    client.subscriptions().pause_collection_async("sub_test", {}, options).result();
    EXPECT_EQ(http.get_recorded_requests().back().body, "pause_collection%5Bbehavior%5D=void");
    client.subscriptions().resume_collection_async("sub_test", options).result();
    EXPECT_EQ(http.get_recorded_requests().back().body, "pause_collection=");
    for (const auto& sent : http.get_recorded_requests()) {
        EXPECT_EQ(sent.method, HttpMethod::Post);
        EXPECT_EQ(sent.options.timeout, options.timeout);
        EXPECT_EQ(sent.headers.at("Idempotency-Key"), "saved-control");
        EXPECT_EQ(sent.headers.at("Stripe-Version"), "2025-03-31.basil");
    }
    int calls = 0;
    const auto callback = [&](CallbackResult<Subscription> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<Subscription>(result));
    };
    client.subscriptions().set_cancel_at_period_end("sub_test", true, options, callback);
    client.subscriptions().pause_collection("sub_test", {}, options, callback);
    client.subscriptions().resume_collection("sub_test", options, callback);
    EXPECT_EQ(calls, 3);
}

TEST_F(CollectionAndInvoicesTest, InvoiceRequestsEncodeFiltersAndReturnTypedConfirmations) {
    const RequestOptions options{ .idempotency_key = "invoice-control",
                                  .timeout = std::chrono::seconds(6) };
    http.set_response(
        { .status_code = 200,
          .body = R"({"object":"list","has_more":true,"data":[)" + invoice_json + "]}" });
    const auto page =
        client.invoices()
            .list_async(
                { .subscription_id = "sub/a &b", .limit = 100, .starting_after = "in?last" },
                options)
            .result();
    ASSERT_EQ(page.data.size(), 1U);
    EXPECT_TRUE(page.has_more);
    EXPECT_EQ(page.data[0].subscription_id, "sub_test");
    EXPECT_EQ(http.get_recorded_requests().back().path,
              "/v1/invoices?limit=100&starting_after=in%3Flast&subscription=sub%2Fa%20%26b");
    EXPECT_EQ(http.get_recorded_requests().size(), 1U);
    int calls = 0;
    client.invoices().list({}, options, [&](CallbackResult<InvoiceList> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<InvoiceList>(result));
    });
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/invoices?limit=10");
    http.set_response({ .status_code = 200, .body = invoice_json });
    EXPECT_FALSE(client.invoices().finalize_async("in/a?", options).result().auto_advance.value());
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/invoices/in%2Fa%3F/finalize");
    EXPECT_EQ(http.get_recorded_requests().back().body, "auto_advance=false");
    EXPECT_EQ(http.get_recorded_requests().back().method, HttpMethod::Post);
    client.invoices().finalize("in_test", options, [&](CallbackResult<Invoice> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<Invoice>(result));
    });
    EXPECT_EQ(client.invoices().void_async("in/a?", options).result().status, "void");
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/invoices/in%2Fa%3F/void");
    EXPECT_EQ(http.get_recorded_requests().back().method, HttpMethod::Post);
    client.invoices().void_invoice("in_test", options, [&](CallbackResult<Invoice> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<Invoice>(result));
    });
    http.set_response({ .status_code = 200, .body = deleted_json });
    EXPECT_TRUE(client.invoices().delete_async("in/a?", options).result().deleted);
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/invoices/in%2Fa%3F");
    EXPECT_EQ(http.get_recorded_requests().back().method, HttpMethod::Delete);
    client.invoices().delete_invoice(
        "in_test", options, [&](CallbackResult<DeletedInvoice> result) {
            ++calls;
            EXPECT_TRUE(std::holds_alternative<DeletedInvoice>(result));
        });
    EXPECT_EQ(calls, 4);
    for (const auto& sent : http.get_recorded_requests()) {
        EXPECT_EQ(sent.body, sent.path.ends_with("/finalize") ? "auto_advance=false" : "");
        EXPECT_EQ(sent.options.timeout, options.timeout);
        EXPECT_EQ(sent.options.idempotency_key, options.idempotency_key);
    }
    http.set_response(
        { .status_code = 200, .body = R"({"object":"list","has_more":false,"data":[]})" });
    EXPECT_TRUE(client.invoices().list_async().result().data.empty());
}

TEST_F(CollectionAndInvoicesTest, ValidationErrorsNeverSendAndCallbacksReceiveFailures) {
    EXPECT_THROW(client.subscriptions().set_cancel_at_period_end_async("", true).result(),
                 ConfigurationException);
    EXPECT_THROW(client.subscriptions().pause_collection_async("", {}).result(),
                 ConfigurationException);
    EXPECT_THROW(client.subscriptions().resume_collection_async("").result(),
                 ConfigurationException);
    EXPECT_THROW(client.invoices().void_async("").result(), ConfigurationException);
    EXPECT_THROW(client.invoices().finalize_async("").result(), ConfigurationException);
    EXPECT_THROW(client.invoices().delete_async("").result(), ConfigurationException);
    for (const auto limit : { 0, 101 })
        EXPECT_THROW(client.invoices().list_async({ .limit = limit }).result(),
                     ConfigurationException);
    EXPECT_THROW(client.subscriptions()
                     .pause_collection_async("sub", { CollectionPauseBehavior::Void, 0 })
                     .result(),
                 ConfigurationException);
    EXPECT_THROW(
        client.subscriptions()
            .pause_collection_async("sub", { static_cast<CollectionPauseBehavior>(99), {} })
            .result(),
        ConfigurationException);
    int calls = 0;
    const auto sub = [&](CallbackResult<Subscription> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    };
    client.subscriptions().set_cancel_at_period_end("", true, {}, sub);
    client.subscriptions().pause_collection("", {}, {}, sub);
    client.subscriptions().pause_collection("sub", { CollectionPauseBehavior::Void, -1 }, {}, sub);
    client.subscriptions().resume_collection("", {}, sub);
    client.invoices().list({ .limit = 0 }, {}, [&](CallbackResult<InvoiceList> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    });
    client.invoices().void_invoice("", {}, [&](CallbackResult<Invoice> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    });
    client.invoices().finalize("", {}, [&](CallbackResult<Invoice> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    });
    client.invoices().delete_invoice("", {}, [&](CallbackResult<DeletedInvoice> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    });
    EXPECT_EQ(calls, 8);
    EXPECT_TRUE(http.get_recorded_requests().empty());
}

TEST_F(CollectionAndInvoicesTest, ParsersRejectMalformedPausesPagesAndDeletionConfirmations) {
    JsonStripeResponseParser parser;
    EXPECT_FALSE(parser.parse_invoice({ .body = R"({"id":"in"})" }).auto_advance.has_value());
    EXPECT_TRUE(parser.parse_invoice({ .body = R"({"id":"in","auto_advance":true})" })
                    .auto_advance.value());
    EXPECT_THROW(parser.parse_invoice({ .body = R"({"id":"in","auto_advance":"false"})" }),
                 ConfigurationException);
    EXPECT_FALSE(parser
                     .parse_subscription(
                         { .body = R"({"id":"sub","status":"active","pause_collection":null})" })
                     .pause_collection.has_value());
    const auto future = parser.parse_subscription(
        { .body = R"({"id":"sub","status":"active","pause_collection":{"behavior":"future"}})" });
    EXPECT_EQ(future.pause_collection->behavior, "future");
    EXPECT_FALSE(future.pause_collection->resumes_at_unix.has_value());
    for (const auto* pause :
         { "false", R"({"behavior":3})", R"({"behavior":"void","resumes_at":-1})" }) {
        EXPECT_THROW(
            parser.parse_subscription(
                { .body = std::string(R"({"id":"sub","status":"active","pause_collection":)") +
                          pause + "}" }),
            ConfigurationException);
    }
    for (const auto* body : { "{}",
                              R"({"object":"invoice","id":"in","deleted":false})",
                              R"({"object":"invoice","deleted":true})",
                              R"({"object":"invoice","id":"in","deleted":"true"})" })
        EXPECT_THROW(parser.parse_deleted_invoice({ .body = body }), ConfigurationException);
    for (const auto* body :
         { "{}",
           R"({"object":"list","has_more":true,"data":[]})",
           R"({"object":"list","has_more":false,"data":[{"object":"invoice","id":"in"}]})",
           R"({"object":"list","has_more":"false","data":[]})" })
        EXPECT_THROW(parser.parse_invoice_list({ .body = body }), ConfigurationException);
}

TEST_F(CollectionAndInvoicesTest, EveryOperationPropagatesProviderTransportAndParsingFailures) {
    int calls = 0;
    const auto sub = [&](CallbackResult<Subscription> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    };
    for (int failure = 0; failure < 3; ++failure) {
        const auto configure = [&] {
            if (failure == 0)
                http.set_response(
                    { .status_code = 429,
                      .headers = { { "request-id", "req_controls" } },
                      .body = R"({"error":{"type":"rate_limit_error","message":"Retry"}})" });
            else if (failure == 1)
                http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
            else http.set_response({ .status_code = 200, .body = "not json" });
        };
        configure();
        EXPECT_THROW(client.subscriptions().set_cancel_at_period_end_async("sub", true).result(),
                     std::exception);
        configure();
        EXPECT_THROW(client.subscriptions().pause_collection_async("sub", {}).result(),
                     std::exception);
        configure();
        EXPECT_THROW(client.subscriptions().resume_collection_async("sub").result(),
                     std::exception);
        configure();
        EXPECT_THROW(client.invoices().list_async().result(), std::exception);
        configure();
        EXPECT_THROW(client.invoices().void_async("in").result(), std::exception);
        configure();
        EXPECT_THROW(client.invoices().finalize_async("in").result(), std::exception);
        configure();
        EXPECT_THROW(client.invoices().delete_async("in").result(), std::exception);
        configure();
        client.subscriptions().set_cancel_at_period_end("sub", true, {}, sub);
        configure();
        client.subscriptions().pause_collection("sub", {}, {}, sub);
        configure();
        client.subscriptions().resume_collection("sub", {}, sub);
        configure();
        client.invoices().list({}, {}, [&](CallbackResult<InvoiceList> result) {
            ++calls;
            EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
        });
        configure();
        client.invoices().void_invoice("in", {}, [&](CallbackResult<Invoice> result) {
            ++calls;
            EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
        });
        configure();
        client.invoices().finalize("in", {}, [&](CallbackResult<Invoice> result) {
            ++calls;
            EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
        });
        configure();
        client.invoices().delete_invoice("in", {}, [&](CallbackResult<DeletedInvoice> result) {
            ++calls;
            EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
        });
    }
    EXPECT_EQ(calls, 21);
}
