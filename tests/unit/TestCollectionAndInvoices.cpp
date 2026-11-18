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
const std::string invoice_item_json = R"({
    "object":"invoiceitem","id":"ii_credit","customer":{"id":"cus/a &b"},"invoice":null,
    "amount":-1500,"currency":"usd","period":{"start":1800000000,"end":1802678400},
    "proration":true,"parent":{"type":"subscription_details","subscription_details":{
        "subscription":"sub_test","subscription_item":"si_test"}},
    "proration_details":{"credited_items":{"type":"invoice_line_items",
        "invoice_line_item_details":{"invoice":"in_held","invoice_line_items":["il_service"]}}},
    "metadata":{"operation":"pause_123"}
})";
} // namespace

class CollectionAndInvoicesTest : public ::testing::Test {
protected:
    MockHttpClient http;
    StripeApiClient client{ http };
};

TEST_F(CollectionAndInvoicesTest, InvoiceItemsExposeSourcesAndEncodeFiltersInBothApiStyles) {
    const RequestOptions options{ .timeout = std::chrono::seconds(6),
                                  .headers = { { "Stripe-Version", "2026-10-28.endive" } } };
    auto& api = client.invoice_items();

    // A credit must retain its owner, service period, and original debit references.
    http.set_response({ .status_code = 200, .body = invoice_item_json });
    const auto item = api.retrieve_async("ii/a?", options).result();
    EXPECT_EQ(item.id, "ii_credit");
    EXPECT_EQ(item.customer_id, "cus/a &b");
    EXPECT_TRUE(item.invoice_id.empty());
    EXPECT_EQ(item.amount, -1500);
    EXPECT_EQ(item.currency, "usd");
    EXPECT_EQ(item.period.start_unix, 1800000000);
    EXPECT_EQ(item.period.end_unix, 1802678400);
    EXPECT_TRUE(item.proration);
    ASSERT_TRUE(item.parent.has_value());
    EXPECT_EQ(item.parent->type, "subscription_details");
    ASSERT_TRUE(item.parent->subscription_details.has_value());
    EXPECT_EQ(item.parent->subscription_details->subscription_id, "sub_test");
    EXPECT_EQ(item.parent->subscription_details->subscription_item_id, "si_test");
    ASSERT_TRUE(item.proration_details.has_value());
    ASSERT_TRUE(item.proration_details->credited_items.has_value());
    const auto& source = *item.proration_details->credited_items;
    EXPECT_EQ(source.type, "invoice_line_items");
    ASSERT_TRUE(source.invoice_line_item_details.has_value());
    EXPECT_EQ(source.invoice_line_item_details->invoice_id, "in_held");
    EXPECT_EQ(source.invoice_line_item_details->invoice_line_item_ids,
              std::vector<std::string>{ "il_service" });
    EXPECT_EQ(item.metadata.at("operation"), "pause_123");
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/invoiceitems/ii%2Fa%3F");
    EXPECT_EQ(http.get_recorded_requests().back().method, HttpMethod::Get);
    int calls = 0;
    api.retrieve("ii/a?", options, [&](CallbackResult<InvoiceItem> result) {
        ++calls;
        EXPECT_EQ(std::get<InvoiceItem>(result).id, item.id);
    });

    // Both API styles must send the same filters, including an explicit pending=false.
    http.set_response({ .status_code = 200, .body = R"({"object":"list","has_more":true,"data":[{
              "object":"invoiceitem","id":"ii_assigned","customer":"cus/a &b","invoice":"in?held",
              "amount":300,"currency":"usd","period":{"start":1800000000,"end":1802678400},
              "proration":false}]})" });
    const ListInvoiceItemsRequest request{
        .invoice_id = "in?held", .pending = false, .limit = 1, .starting_after = "ii?last"
    };
    const auto page = api.list_async(request, options).result();
    EXPECT_TRUE(page.has_more);
    ASSERT_EQ(page.data.size(), 1U);
    EXPECT_EQ(page.data.front().id, "ii_assigned");
    EXPECT_EQ(page.data.front().invoice_id, request.invoice_id);
    EXPECT_EQ(http.get_recorded_requests().size(), 3U);
    EXPECT_EQ(http.get_recorded_requests().back().path,
              "/v1/invoiceitems?limit=1&starting_after=ii%3Flast&invoice=in%3Fheld&pending=false");
    api.list(request, options, [&](CallbackResult<InvoiceItemList> result) {
        ++calls;
        EXPECT_TRUE(std::get<InvoiceItemList>(result).has_more);
        EXPECT_EQ(std::get<InvoiceItemList>(result).data.front().invoice_id, request.invoice_id);
    });
    EXPECT_EQ(http.get_recorded_requests().back().path, http.get_recorded_requests()[2].path);

    // Distinguish pending-only requests from leaving the pending filter unset.
    http.set_response(
        { .status_code = 200,
          .body = R"({"object":"list","has_more":false,"data":[)" + invoice_item_json + "]}" });
    const auto pending =
        api.list_async({ .customer_id = "cus/a &b", .pending = true }, options).result();
    ASSERT_EQ(pending.data.size(), 1U);
    EXPECT_TRUE(pending.data.front().invoice_id.empty());
    EXPECT_EQ(pending.data.front().customer_id, "cus/a &b");
    EXPECT_EQ(http.get_recorded_requests().back().path,
              "/v1/invoiceitems?limit=10&customer=cus%2Fa%20%26b&pending=true");
    http.set_response(
        { .status_code = 200, .body = R"({"object":"list","has_more":false,"data":[]})" });
    EXPECT_TRUE(api.list_async({}, options).result().data.empty());
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/invoiceitems?limit=10");

    // Deletion returns Stripe's confirmation, not a locally assumed success.
    http.set_response({ .status_code = 200,
                        .body = R"({"object":"invoiceitem","id":"ii_credit","deleted":true})" });
    const auto deleted = api.delete_async("ii/a?", options).result();
    EXPECT_EQ(deleted.id, item.id);
    EXPECT_TRUE(deleted.deleted);
    EXPECT_EQ(http.get_recorded_requests().back().method, HttpMethod::Delete);
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/invoiceitems/ii%2Fa%3F");
    api.delete_invoice_item("ii/a?", options, [&](CallbackResult<DeletedInvoiceItem> result) {
        ++calls;
        EXPECT_TRUE(std::get<DeletedInvoiceItem>(result).deleted);
        EXPECT_EQ(std::get<DeletedInvoiceItem>(result).id, item.id);
    });
    EXPECT_EQ(calls, 3);
    for (const auto& sent : http.get_recorded_requests()) {
        EXPECT_TRUE(sent.body.empty());
        EXPECT_EQ(sent.options.timeout, options.timeout);
        EXPECT_EQ(sent.headers.at("Stripe-Version"), "2026-10-28.endive");
    }
}

TEST_F(CollectionAndInvoicesTest, InvoiceItemListPreservesStripeFilterErrorsInBothApiStyles) {
    // Stripe rejects combining these filters; neither API style may turn that into an empty page.
    http.set_response({ .status_code = 400, .body = R"({"error":{"type":"invalid_request_error",
                            "message":"The customer and invoice filters cannot be combined."}})" });
    const ListInvoiceItemsRequest request{ .customer_id = "cus_test", .invoice_id = "in_test" };
    auto& api = client.invoice_items();
    try {
        api.list_async(request).result();
        FAIL();
    } catch (const StripeApiException& error) {
        EXPECT_EQ(error.payload().http_status, 400);
        EXPECT_EQ(error.payload().type, StripeErrorType::InvalidRequest);
    }
    int calls = 0;
    api.list(request, {}, [&](CallbackResult<InvoiceItemList> result) {
        ++calls;
        ASSERT_TRUE(std::holds_alternative<std::exception_ptr>(result));
        try {
            std::rethrow_exception(std::get<std::exception_ptr>(result));
            FAIL();
        } catch (const StripeApiException& error) {
            EXPECT_EQ(error.payload().http_status, 400);
            EXPECT_EQ(error.payload().type, StripeErrorType::InvalidRequest);
        }
    });
    EXPECT_EQ(calls, 1);
    ASSERT_EQ(http.get_recorded_requests().size(), 2U);
    for (const auto& sent : http.get_recorded_requests()) {
        EXPECT_EQ(sent.path, "/v1/invoiceitems?limit=10&customer=cus_test&invoice=in_test");
    }
}

TEST_F(CollectionAndInvoicesTest, InvoiceItemSourcesPreserveAbsenceAndPendingDebitReferences) {
    JsonStripeResponseParser parser;
    const std::string base = R"("object":"invoiceitem","id":"ii","customer":"cus",
        "amount":-200,"currency":"usd","period":{"start":0,"end":0},"proration":false)";

    // A negative amount must not cause missing ownership or credit sources to be invented.
    auto item = parser.parse_invoice_item({ .body = "{" + base + "}" });
    EXPECT_FALSE(item.parent.has_value());
    EXPECT_FALSE(item.proration_details.has_value());
    EXPECT_TRUE(item.metadata.empty());

    // A credit can refer to a still-pending debit rather than an invoiced line.
    item = parser.parse_invoice_item({ .body = "{" + base + R"(,
        "invoice":{"id":"in_draft"},"parent":null,
        "proration_details":{"credited_items":{"type":"invoice_item","invoice_item":"ii_debit"}}})" });
    EXPECT_EQ(item.invoice_id, "in_draft");
    EXPECT_EQ(item.proration_details->credited_items->invoice_item_id, "ii_debit");
    EXPECT_FALSE(item.proration_details->credited_items->invoice_line_item_details.has_value());

    // Preserve future source types, but reject malformed known fields instead of hiding them.
    item = parser.parse_invoice_item({ .body = "{" + base + R"(,
        "parent":{"type":"future_source"},"proration_details":{"credited_items":null}})" });
    EXPECT_EQ(item.parent->type, "future_source");
    EXPECT_FALSE(item.parent->subscription_details.has_value());
    EXPECT_FALSE(item.proration_details->credited_items.has_value());
    for (
        const auto* fields :
        { R"("parent":false)",
          R"("parent":{"type":"subscription_details"})",
          R"("parent":{"type":"subscription_details","subscription_details":{"subscription":false}})",
          R"("proration_details":false)",
          R"("proration_details":{"credited_items":{"type":"invoice_item"}})",
          R"("proration_details":{"credited_items":{"type":"invoice_line_items","invoice_line_item_details":{"invoice":"in","invoice_line_items":[null]}}})",
          R"("metadata":{"operation":42})",
          R"("invoice":false)" }) {
        EXPECT_THROW(parser.parse_invoice_item({ .body = "{" + base + "," + fields + "}" }),
                     ConfigurationException);
    }
    for (const auto* body : { "{}",
                              R"({"object":"invoiceitem","id":"ii","deleted":true})",
                              R"({"object":"invoiceitem","id":"ii","customer":"cus","amount":-2,
                                 "currency":"usd","proration":true,"period":{"start":5,"end":4}})" }) {
        EXPECT_THROW(parser.parse_invoice_item({ .body = body }), ConfigurationException);
    }
    for (const auto* body :
         { R"({"object":"list","has_more":true,"data":[]})", R"({"object":"list","data":[]})" }) {
        EXPECT_THROW(parser.parse_invoice_item_list({ .body = body }), ConfigurationException);
    }

    // Missing, false, or wrongly typed deletion flags are not confirmations.
    for (const auto* fields :
         { R"("deleted":false)", R"("deleted":null)", R"("deleted":"true")" }) {
        EXPECT_THROW(
            parser.parse_deleted_invoice_item(
                { .body = std::string(R"({"object":"invoiceitem","id":"ii",)") + fields + "}" }),
            ConfigurationException);
    }
    EXPECT_THROW(parser.parse_deleted_invoice_item(
                     { .body = R"({"object":"invoice","id":"ii","deleted":true})" }),
                 ConfigurationException);
}

TEST_F(CollectionAndInvoicesTest, InvoiceItemFailuresReachBothApiStylesIncludingNotFound) {
    auto& api = client.invoice_items();
    int failures = 0;
    const auto failure = [&](auto result) {
        ++failures;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    };

    // Invalid local input must fail before any request reaches Stripe.
    EXPECT_THROW(api.retrieve_async("").result(), ConfigurationException);
    EXPECT_THROW(api.delete_async("").result(), ConfigurationException);
    api.retrieve("", {}, failure);
    api.delete_invoice_item("", {}, failure);
    for (const auto limit : { 0, 101 }) {
        EXPECT_THROW(api.list_async({ .limit = limit }).result(), ConfigurationException);
        api.list({ .limit = limit }, {}, failure);
    }
    EXPECT_EQ(failures, 4);
    EXPECT_TRUE(http.get_recorded_requests().empty());

    // Exercise provider rejection, transport failure, and malformed success in both API styles.
    for (int kind = 0; kind < 3; ++kind) {
        const auto configure = [&] {
            if (kind == 0)
                http.set_response(
                    { .status_code = 404, .body = R"({"error":{"type":"invalid_request_error",
                                        "code":"resource_missing","message":"No such invoiceitem"}})" });
            else if (kind == 1)
                http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
            else http.set_response({ .status_code = 200, .body = "{}" });
        };
        configure();
        EXPECT_THROW(api.retrieve_async("ii_missing").result(), std::exception);
        configure();
        EXPECT_THROW(api.delete_async("ii_missing").result(), std::exception);
        configure();
        EXPECT_THROW(api.list_async().result(), std::exception);
        configure();
        api.retrieve("ii_missing", {}, failure);
        configure();
        api.delete_invoice_item("ii_missing", {}, failure);
        configure();
        api.list({}, {}, failure);
    }
    EXPECT_EQ(failures, 13);

    // A missing item remains a Stripe error with its original machine-readable code.
    http.set_response({ .status_code = 404, .body = R"({"error":{"type":"invalid_request_error",
                            "code":"resource_missing","message":"No such invoiceitem"}})" });
    try {
        api.delete_async("ii_missing").result();
        FAIL();
    } catch (const StripeApiException& error) {
        EXPECT_EQ(error.payload().http_status, 404);
        EXPECT_EQ(error.payload().code, "resource_missing");
    }
}

TEST_F(CollectionAndInvoicesTest, SubscriptionRetrievalExpandsBilledUntilWithoutInventingDates) {
    auto& api = client.subscriptions();
    const RequestOptions options{ .timeout = std::chrono::seconds(8),
                                  .headers = { { "Stripe-Version", "2026-10-28.endive" } } };

    // Billed-through dates can differ from period ends; missing and null dates stay absent.
    http.set_response({ .status_code = 200, .body = R"({"id":"sub","status":"paused","items":{
        "has_more":false,"data":[{"id":"si","price":"price","current_period_end":1900000000,
            "billed_until":1800000000},{"id":"si_absent","price":"price","current_period_end":1900000000},
            {"id":"si_null","price":"price","current_period_end":1900000000,"billed_until":null}]}})" });
    const RetrieveSubscriptionRequest request{ .expand = { "items.data.billed_until",
                                                           "latest_invoice" } };
    const auto saved = api.retrieve_async("sub/a?", request, options).result();
    ASSERT_EQ(saved.items.size(), 3U);
    EXPECT_EQ(saved.items[0].billed_until_unix.value(), 1800000000);
    EXPECT_FALSE(saved.items[1].billed_until_unix.has_value());
    EXPECT_FALSE(saved.items[2].billed_until_unix.has_value());
    EXPECT_EQ(http.get_recorded_requests().back().path,
              "/v1/subscriptions/sub%2Fa%3F?expand%5B%5D=items.data.billed_until"
              "&expand%5B%5D=latest_invoice");

    // Expansion paths and per-request settings must survive either calling style.
    int calls = 0;
    api.retrieve("sub/a?", request, options, [&](CallbackResult<Subscription> result) {
        ++calls;
        EXPECT_EQ(std::get<Subscription>(result).items.front().billed_until_unix.value(),
                  1800000000);
    });
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(http.get_recorded_requests().front().path, http.get_recorded_requests().back().path);
    for (const auto& sent : http.get_recorded_requests()) {
        EXPECT_EQ(sent.method, HttpMethod::Get);
        EXPECT_TRUE(sent.body.empty());
        EXPECT_EQ(sent.options.timeout, options.timeout);
        EXPECT_EQ(sent.headers.at("Stripe-Version"), "2026-10-28.endive");
    }

    // Keep the original retrieval overload usable, while rejecting empty expansion paths.
    api.retrieve_async("sub", {}).result();
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/subscriptions/sub");
    EXPECT_THROW(api.retrieve_async("sub", { .expand = { "" } }, {}).result(),
                 ConfigurationException);
    api.retrieve("sub", { .expand = { "" } }, {}, [&](CallbackResult<Subscription> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    });
    EXPECT_EQ(calls, 2);

    // Invalid timestamps must fail parsing rather than become a guessed date or absence.
    JsonStripeResponseParser parser;
    for (const auto* value : { "\"1800000000\"", "-1", "false", "{}" }) {
        EXPECT_THROW(
            parser.parse_subscription(
                { .body = std::string(R"({"id":"sub","status":"paused","items":{"has_more":false,
                             "data":[{"id":"si","price":"price","billed_until":)") +
                          value + "}]}}" }),
            ConfigurationException);
    }
}

TEST_F(CollectionAndInvoicesTest, TruePauseResumeAndMigrationUseDedicatedEndpoints) {
    http.set_response({ .status_code = 200, .body = R"({
        "id":"sub_test","status":"paused","billing_mode":{"type":"flexible"},
        "collection_method":"charge_automatically","latest_invoice":{"id":"in_resume"},
        "pending_update":{"expires_at":2000000000},
        "status_details":{"paused":{"type":"subscription","transitioned_at":1800000000,
            "subscription":{"type":"pause_requested"}}}
    })" });
    const RequestOptions options{ .idempotency_key = "pause-operation",
                                  .timeout = std::chrono::seconds(8),
                                  .headers = { { "Stripe-Version", "2026-10-28.endive" } } };
    auto& api = client.subscriptions();
    const PauseSubscriptionRequest pause{ .bill_for = { .unused_time_from = { "now" },
                                                        .outstanding_usage_through = { "none" } },
                                          .invoicing_behavior = "invoice",
                                          .type = "subscription" };
    const auto paused = api.pause_async("sub/a?", pause, options).result();
    EXPECT_EQ(paused.status, "paused");
    EXPECT_EQ(paused.billing_mode->type, "flexible");
    EXPECT_EQ(paused.collection_method, "charge_automatically");
    EXPECT_EQ(paused.status_details->paused->subscription.type, "pause_requested");
    EXPECT_EQ(paused.status_details->paused->transitioned_at_unix, 1800000000);
    EXPECT_FALSE(paused.pause_collection.has_value());
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/subscriptions/sub%2Fa%3F/pause");
    EXPECT_EQ(http.get_recorded_requests().back().body,
              "bill_for%5Bunused_time_from%5D%5Btype%5D=now"
              "&bill_for%5Boutstanding_usage_through%5D%5Btype%5D=none"
              "&invoicing_behavior=invoice&type=subscription");
    const ResumeSubscriptionRequest resume{ .billing_cycle_anchor =
                                                ResumeSubscriptionBillingCycleAnchor{ "unchanged" },
                                            .payment_behavior = "resume_on_payment_success",
                                            .proration_behavior = ProrationBehavior::AlwaysInvoice,
                                            .proration_date = 1800000000 };
    const auto pending = api.resume_async("sub/a?", resume, options).result();
    EXPECT_EQ(pending.status, "paused");
    EXPECT_EQ(pending.pending_update->expires_at_unix, 2000000000);
    EXPECT_EQ(pending.latest_invoice_id, "in_resume");
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/subscriptions/sub%2Fa%3F/resume");
    EXPECT_EQ(http.get_recorded_requests().back().body,
              "billing_cycle_anchor%5Btype%5D=unchanged&payment_behavior=resume_on_payment_success"
              "&proration_behavior=always_invoice&proration_date=1800000000");
    api.migrate_async("sub/a?", {}, options).result();
    EXPECT_EQ(http.get_recorded_requests().back().path, "/v1/subscriptions/sub%2Fa%3F/migrate");
    EXPECT_EQ(http.get_recorded_requests().back().body, "billing_mode%5Btype%5D=flexible");
    int calls = 0;
    const auto completion = [&](CallbackResult<Subscription> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<Subscription>(result));
    };
    api.pause("sub/a?", pause, options, completion);
    EXPECT_EQ(http.get_recorded_requests().back().body, http.get_recorded_requests()[0].body);
    api.resume("sub/a?", resume, options, completion);
    EXPECT_EQ(http.get_recorded_requests().back().body, http.get_recorded_requests()[1].body);
    api.migrate("sub/a?", {}, options, completion);
    EXPECT_EQ(calls, 3);
    for (const auto& sent : http.get_recorded_requests()) {
        EXPECT_EQ(sent.method, HttpMethod::Post);
        EXPECT_EQ(sent.options.timeout, options.timeout);
        EXPECT_EQ(sent.headers.at("Idempotency-Key"), "pause-operation");
        EXPECT_EQ(sent.headers.at("Stripe-Version"), "2026-10-28.endive");
    }
    api.pause_async("sub_test").result();
    EXPECT_TRUE(http.get_recorded_requests().back().body.empty());
    api.resume_async("sub_test").result();
    EXPECT_TRUE(http.get_recorded_requests().back().body.empty());
    api.pause_async("sub_test",
                    { .bill_for = { .unused_time_from = { "item_current_period_start" },
                                    .outstanding_usage_through = { "none" } },
                      .invoicing_behavior = "pending_invoice_item" })
        .result();
    EXPECT_EQ(http.get_recorded_requests().back().body,
              "bill_for%5Bunused_time_from%5D%5Btype%5D=item_current_period_start"
              "&bill_for%5Boutstanding_usage_through%5D%5Btype%5D=none"
              "&invoicing_behavior=pending_invoice_item");
}

TEST_F(CollectionAndInvoicesTest, TruePauseOperationsRejectInvalidInputAndPropagateFailures) {
    auto& api = client.subscriptions();
    int calls = 0;
    const auto failure = [&](CallbackResult<Subscription> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    };
    EXPECT_THROW(api.pause_async("").result(), ConfigurationException);
    EXPECT_THROW(api.resume_async("").result(), ConfigurationException);
    EXPECT_THROW(api.migrate_async("").result(), ConfigurationException);
    api.pause("", {}, {}, failure);
    api.resume("", {}, {}, failure);
    api.migrate("", {}, {}, failure);
    for (const auto& request : std::vector<PauseSubscriptionRequest>{
             { .bill_for = { .unused_time_from = { "pause_at" } } },
             { .bill_for = { .outstanding_usage_through = { "item_current_period_start" } } },
             { .invoicing_behavior = "invalid" },
             { .type = "collection" } }) {
        EXPECT_THROW(api.pause_async("sub", request).result(), ConfigurationException);
        api.pause("sub", request, {}, failure);
    }
    for (const auto& request : std::vector<ResumeSubscriptionRequest>{
             { .billing_cycle_anchor = ResumeSubscriptionBillingCycleAnchor{ "resume_at" } },
             { .billing_cycle_anchor = ResumeSubscriptionBillingCycleAnchor{} },
             { .payment_behavior = "allow_incomplete" },
             { .proration_behavior = static_cast<ProrationBehavior>(99) },
             { .proration_date = 0 } }) {
        EXPECT_THROW(api.resume_async("sub", request).result(), ConfigurationException);
        api.resume("sub", request, {}, failure);
    }
    EXPECT_THROW(api.migrate_async("sub", { { "classic" } }).result(), ConfigurationException);
    api.migrate("sub", { { "classic" } }, {}, failure);
    EXPECT_EQ(calls, 13);
    EXPECT_TRUE(http.get_recorded_requests().empty());
    for (int kind = 0; kind < 3; ++kind) {
        const auto configure = [&] {
            if (kind == 0)
                http.set_response(
                    { .status_code = 400,
                      .body =
                          R"({"error":{"type":"invalid_request_error","message":"Not eligible"}})" });
            else if (kind == 1)
                http.set_exception(std::make_exception_ptr(std::runtime_error("Offline")));
            else http.set_response({ .status_code = 200, .body = "{}" });
        };
        configure();
        EXPECT_THROW(api.pause_async("sub").result(), std::exception);
        configure();
        EXPECT_THROW(api.resume_async("sub").result(), std::exception);
        configure();
        EXPECT_THROW(api.migrate_async("sub").result(), std::exception);
        configure();
        api.pause("sub", {}, {}, failure);
        configure();
        api.resume("sub", {}, {}, failure);
        configure();
        api.migrate("sub", {}, {}, failure);
    }
    EXPECT_EQ(calls, 22);
}

TEST_F(CollectionAndInvoicesTest, PauseStatusDetailsRejectMalformedDataAndPreserveUnknownReasons) {
    JsonStripeResponseParser parser;
    const auto parsed = parser.parse_subscription({ .body = R"({
        "id":"sub","status":"paused","billing_mode":"future_mode",
        "status_details":{"paused":{"type":"subscription","transitioned_at":1800000000,
            "subscription":{"type":"future_reason"}}}
    })" });
    EXPECT_EQ(parsed.billing_mode->type, "future_mode");
    EXPECT_EQ(parsed.status_details->paused->subscription.type, "future_reason");
    const auto absent = parser.parse_subscription({ .body = R"({"id":"sub","status":"active"})" });
    EXPECT_FALSE(absent.billing_mode.has_value());
    EXPECT_FALSE(absent.status_details.has_value());
    for (
        const auto* fields :
        { R"("billing_mode":false)",
          R"("billing_mode":{})",
          R"("collection_method":false)",
          R"("status_details":false)",
          R"("status_details":{"paused":false})",
          R"("status_details":{"paused":{"type":"subscription"}})",
          R"("status_details":{"paused":{"type":"subscription","transitioned_at":"1800000000"}})",
          R"("status_details":{"paused":{"type":"subscription","transitioned_at":1800000000,"subscription":false}})" }) {
        EXPECT_THROW(
            parser.parse_subscription(
                { .body = std::string(R"({"id":"sub","status":"paused",)") + fields + "}" }),
            ConfigurationException);
    }
}

TEST_F(CollectionAndInvoicesTest, CheckoutBillingModeAndCollectionMethodAreExplicitChoices) {
    http.set_response({ .status_code = 200, .body = R"({"id":"cs","status":"open"})" });
    CreateCheckoutSessionRequest checkout;
    checkout.subscription_billing_mode = SubscriptionBillingMode{ "flexible" };
    client.checkout().sessions().create_async(checkout).result();
    EXPECT_NE(http.get_recorded_requests().back().body.find(
                  "subscription_data%5Bbilling_mode%5D%5Btype%5D=flexible"),
              std::string::npos);
    checkout.mode = "payment";
    EXPECT_THROW(client.checkout().sessions().create_async(checkout).result(),
                 ConfigurationException);
    checkout.mode = "subscription";
    checkout.subscription_billing_mode->type = "invalid";
    EXPECT_THROW(client.checkout().sessions().create_async(checkout).result(),
                 ConfigurationException);
    http.set_response({ .status_code = 200, .body = subscription_json });
    client.subscriptions()
        .update_async("sub", { .collection_method = "charge_automatically" })
        .result();
    EXPECT_EQ(http.get_recorded_requests().back().body, "collection_method=charge_automatically");
    EXPECT_THROW(client.subscriptions().update_async("sub", { .collection_method = "" }).result(),
                 ConfigurationException);
}

TEST_F(CollectionAndInvoicesTest, SubscriptionControlsEncodeBodiesAndPreserveOptions) {
    http.set_response({ .status_code = 200, .body = subscription_json });
    const RequestOptions options{ .idempotency_key = "saved-control",
                                  .timeout = std::chrono::seconds(8),
                                  .headers = { { "Stripe-Version", "2026-10-28.endive" } } };
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
        EXPECT_EQ(sent.headers.at("Stripe-Version"), "2026-10-28.endive");
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

TEST_F(CollectionAndInvoicesTest, InvoicePreviewUsesStripeEndpointAndReturnsTypedPreview) {
    const auto response =
        R"({"object":"invoice","id":"upcoming_in_test","customer":"cus_test","subscription":"sub_test",
            "status":"draft","currency":"usd","amount_due":23400,"amount_paid":0,
            "amount_remaining":23400,"subtotal":23400,"total":23400,
            "lines":{"object":"list","has_more":false,"data":[
                {"id":"il_test","pricing":{"price_details":{"price":"price_test"}},
                 "quantity":3,"amount":23400}]}})";
    const RequestOptions options{ .idempotency_key = "preview-request",
                                  .timeout = std::chrono::seconds(6) };
    http.set_response({ .status_code = 200, .body = response });

    const auto invoice =
        client.invoices().create_preview_async({ .subscription_id = "sub/a &b" }, options).result();
    EXPECT_EQ(invoice.id, "upcoming_in_test");
    EXPECT_EQ(invoice.subscription_id, "sub_test");
    EXPECT_EQ(invoice.amount_due, 23400);
    ASSERT_EQ(invoice.line_items.size(), 1U);
    EXPECT_EQ(invoice.line_items.front().price_id, "price_test");
    EXPECT_EQ(invoice.line_items.front().amount, 23400);

    const auto sent = http.get_recorded_requests().back();
    EXPECT_EQ(sent.path, "/v1/invoices/create_preview");
    EXPECT_EQ(sent.method, HttpMethod::Post);
    EXPECT_EQ(sent.body, "subscription=sub%2Fa%20%26b");
    EXPECT_EQ(sent.options.idempotency_key, options.idempotency_key);
    EXPECT_EQ(sent.options.timeout, options.timeout);

    int calls = 0;
    client.invoices().create_preview(
        { .subscription_id = "sub_callback" }, options, [&](CallbackResult<Invoice> result) {
            ++calls;
            ASSERT_TRUE(std::holds_alternative<Invoice>(result));
            EXPECT_EQ(std::get<Invoice>(result).id, "upcoming_in_test");
        });
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(http.get_recorded_requests().back().body, "subscription=sub_callback");
}

TEST_F(CollectionAndInvoicesTest, DraftInvoiceTaxSettingsCanBeUpdatedBeforeFinalization) {
    http.set_response(
        { .status_code = 200,
          .body =
              R"({"object":"invoice","id":"in_tax","status":"draft","automatic_tax":{"enabled":true,"status":"requires_location_inputs"}})" });
    const auto invoice = client.invoices()
                             .update_async("in_tax",
                                           { .automatic_tax_enabled = true, .auto_advance = false },
                                           { .idempotency_key = "tax-recovery" })
                             .result();
    EXPECT_EQ(invoice.id, "in_tax");
    ASSERT_TRUE(invoice.automatic_tax.has_value());
    EXPECT_TRUE(invoice.automatic_tax->enabled);
    EXPECT_EQ(invoice.automatic_tax->status, "requires_location_inputs");
    const auto sent = http.get_recorded_requests().back();
    EXPECT_EQ(sent.path, "/v1/invoices/in_tax");
    EXPECT_EQ(sent.method, HttpMethod::Post);
    EXPECT_EQ(sent.body, "automatic_tax%5Benabled%5D=true&auto_advance=false");
    EXPECT_EQ(sent.headers.at("Idempotency-Key"), "tax-recovery");
    int calls = 0;
    client.invoices().update(
        "in_tax", { .automatic_tax_enabled = false }, {}, [&](CallbackResult<Invoice> result) {
            ++calls;
            EXPECT_TRUE(std::holds_alternative<Invoice>(result));
        });
    EXPECT_EQ(calls, 1);
}

TEST_F(CollectionAndInvoicesTest, InvoiceLineItemPagesExposePriceQuantityAndPaginationIds) {
    http.set_response(
        { .status_code = 200,
          .body =
              R"({"object":"list","has_more":true,"data":[{"id":"il_tax","pricing":{"price_details":{"price":"price_vat"}},"quantity":3,"amount":7500}]})" });
    const auto page =
        client.invoices()
            .list_line_items_async("in/tax",
                                   { .limit = 1, .starting_after = "il_previous" },
                                   { .headers = { { "Stripe-Version", "2026-10-28.endive" } } })
            .result();
    ASSERT_EQ(page.data.size(), 1U);
    EXPECT_TRUE(page.has_more);
    EXPECT_EQ(page.data[0].id, "il_tax");
    EXPECT_EQ(page.data[0].price_id, "price_vat");
    EXPECT_EQ(page.data[0].quantity.value(), 3);
    EXPECT_EQ(page.data[0].amount.value(), 7500);
    const auto sent = http.get_recorded_requests().back();
    EXPECT_EQ(sent.path, "/v1/invoices/in%2Ftax/lines?limit=1&starting_after=il_previous");
    EXPECT_EQ(sent.headers.at("Stripe-Version"), "2026-10-28.endive");
}

TEST_F(CollectionAndInvoicesTest, InvoiceParserExposesTaxTotalsLocationErrorsAndPurchasedPrices) {
    JsonStripeResponseParser parser;
    const auto invoice = parser.parse_invoice({ .body = R"({
            "object":"invoice","id":"in_tax","status":"draft","currency":"usd",
            "subtotal":5000,"total":5499,"amount_due":5499,
            "total_discount_amounts":[{"amount":500}],
            "total_taxes":[{"amount":999}],
            "automatic_tax":{"enabled":false,"status":"requires_location_inputs","disabled_reason":"requires_location_inputs"},
            "last_finalization_error":{"code":"customer_tax_location_invalid"},
            "lines":{"has_more":false,"data":[{"id":"il_service","pricing":{"price_details":{"price":"price_service"}},"quantity":2,"amount":5000}]}
        })" });
    EXPECT_EQ(invoice.subtotal.value(), 5000);
    EXPECT_EQ(invoice.amount_discount.value(), 500);
    EXPECT_EQ(invoice.amount_tax.value(), 999);
    EXPECT_EQ(invoice.total.value(), 5499);
    ASSERT_TRUE(invoice.automatic_tax.has_value());
    EXPECT_FALSE(invoice.automatic_tax->enabled);
    EXPECT_EQ(invoice.automatic_tax->disabled_reason, "requires_location_inputs");
    EXPECT_EQ(invoice.last_finalization_error_code, "customer_tax_location_invalid");
    ASSERT_EQ(invoice.line_items.size(), 1U);
    EXPECT_EQ(invoice.line_items[0].price_id, "price_service");
    EXPECT_EQ(invoice.line_items[0].quantity.value(), 2);
}

TEST_F(CollectionAndInvoicesTest, InvoiceParserRejectsMalformedTaxTotals) {
    JsonStripeResponseParser parser;
    EXPECT_THROW(parser.parse_invoice(
                     { .body = R"({"id":"in_bad_tax","total_taxes":{},"status":"draft"})" }),
                 ConfigurationException);
    EXPECT_THROW(
        parser.parse_invoice(
            { .body = R"({"id":"in_bad_tax","total_taxes":[{"amount":"425"}],"status":"draft"})" }),
        ConfigurationException);
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
    EXPECT_THROW(client.invoices().create_preview_async({}).result(), ConfigurationException);
    EXPECT_THROW(client.invoices().update_async("", { .automatic_tax_enabled = true }).result(),
                 ConfigurationException);
    EXPECT_THROW(client.invoices().update_async("in_test", {}).result(), ConfigurationException);
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
    client.invoices().create_preview({}, {}, [&](CallbackResult<Invoice> result) {
        ++calls;
        EXPECT_TRUE(std::holds_alternative<std::exception_ptr>(result));
    });
    EXPECT_EQ(calls, 9);
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
