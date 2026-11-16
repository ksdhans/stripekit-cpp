/** TestPayments.cpp
 *
 * See SandboxEnvironment.h for details.
 */

#include "SandboxEnvironment.h"
#include "TestFramework.h"
#include "StripeKit/Errors.h"

#include <future>
#include <set>
#include <stdexcept>
#include <thread>

using namespace stripekit;
using namespace stripekit::testing;

class SandboxPaymentsTest : public ::testing::Test {
protected:
    /** Use a fresh customer so list assertions cannot include another run's charges. */
    void SetUp() override {
        environment = &SandboxEnvironment::get();
        customer_id = create_customer_with_card(*environment, "pm_card_visa", "payment_reads");
    }

    /** Close unpaid invoices and delete the test customer, reporting every cleanup failure. */
    void TearDown() override {
        std::string failures;
        const auto cleanup = [&](const std::string& id, auto operation) {
            try {
                operation();
            } catch (const std::exception& error) {
                failures += "\n" + id + ": " + error.what();
            }
        };
        for (const auto& id : invoices) {
            cleanup(id, [&] {
                const auto invoice =
                    environment->stripe_client().invoices().retrieve_async(id).result();
                if (invoice.status == "draft")
                    environment->stripe_api().require_delete("/v1/invoices/" + id);
                else if (invoice.status == "open")
                    environment->stripe_api().require_post("/v1/invoices/" + id + "/void", {});
            });
        }
        cleanup(customer_id,
                [&] { environment->stripe_api().require_delete("/v1/customers/" + customer_id); });
        if (!failures.empty()) throw std::runtime_error("Payment test cleanup failed:" + failures);
    }

    /** Charge the fixture's customer using the requested Stripe sandbox card fixture. */
    std::string create_charge(const std::string& payment_method = "pm_card_visa") {
        const auto intent = environment->stripe_api().require_post(
            "/v1/payment_intents",
            { { "amount", "1500" },
              { "currency", "usd" },
              { "customer", customer_id },
              { "payment_method", payment_method },
              { "automatic_payment_methods[enabled]", "true" },
              { "automatic_payment_methods[allow_redirects]", "never" },
              { "confirm", "true" },
              { "metadata[stripekit_run_id]", environment->run_id() } });
        const auto id = intent.body["latest_charge"].asString();
        if (id.empty()) throw std::runtime_error("Stripe did not return a confirmed charge");
        return id;
    }

    /** Creates and pays an invoice using the supported Stripe API version. */
    StripeApiResult pay_invoice() {
        auto& api = environment->stripe_api();
        const auto invoice =
            api.require_post("/v1/invoices",
                             { { "customer", customer_id },
                               { "currency", "usd" },
                               { "auto_advance", "false" },
                               { "metadata[stripekit_run_id]", environment->run_id() } });
        invoices.push_back(invoice.id());
        api.require_post("/v1/invoiceitems",
                         { { "customer", customer_id },
                           { "invoice", invoice.id() },
                           { "currency", "usd" },
                           { "amount", "1500" } });
        api.require_post("/v1/invoices/" + invoice.id() + "/finalize",
                         { { "auto_advance", "false" } });
        return api.require_post("/v1/invoices/" + invoice.id() + "/pay", {});
    }

    SandboxEnvironment* environment = nullptr;
    std::string customer_id;
    std::vector<std::string> invoices;
};

TEST_F(SandboxPaymentsTest, DeclinedChargeIsReturnedAsFailedAndUnpaid) {
    const auto intent = environment->stripe_api().require_post(
        "/v1/payment_intents",
        { { "amount", "1500" },
          { "currency", "usd" },
          { "customer", customer_id },
          { "automatic_payment_methods[enabled]", "true" },
          { "automatic_payment_methods[allow_redirects]", "never" } });
    const auto declined =
        environment->stripe_api().post("/v1/payment_intents/" + intent.id() + "/confirm",
                                       { { "payment_method", "pm_card_chargeDeclined" } });
    ASSERT_EQ(declined.status_code, 402);
    auto& client = environment->stripe_client();
    const auto page = client.charges().list_async({ .customer_id = customer_id }).result();
    ASSERT_EQ(page.data.size(), 1U);
    EXPECT_FALSE(page.has_more);
    const auto listed = page.data.front();
    const auto retrieved = client.charges().retrieve_async(listed.id).result();
    for (const auto& charge : { listed, retrieved }) {
        EXPECT_EQ(charge.customer_id, customer_id);
        EXPECT_EQ(charge.payment_intent_id, intent.id());
        EXPECT_EQ(charge.status, "failed");
        EXPECT_FALSE(charge.paid);
        EXPECT_FALSE(charge.disputed);
        EXPECT_FALSE(charge.livemode);
        EXPECT_EQ(charge.amount, 1500);
        EXPECT_EQ(charge.amount_refunded, 0);
        EXPECT_EQ(charge.currency, "usd");
    }
}

TEST_F(SandboxPaymentsTest, DisputedChargeRetainsPaymentAndCustomerReferences) {
    const auto charge_id = create_charge("pm_card_createDispute");
    const auto opened = environment->wait_for_event("charge.dispute.created", charge_id);
    ASSERT_TRUE(opened.has_value());
    const auto dispute_id = json_path(opened->json(), "data.object.id").asString();
    ASSERT_FALSE(dispute_id.empty());
    // Close the sandbox dispute before assertions so a failed read leaves no open case.
    environment->stripe_api().require_post("/v1/disputes/" + dispute_id + "/close", {});
    auto& client = environment->stripe_client();
    const auto retrieved = client.charges().retrieve_async(charge_id).result();
    const auto page = client.charges().list_async({ .customer_id = customer_id }).result();
    ASSERT_EQ(page.data.size(), 1U);
    EXPECT_EQ(page.data.front().id, charge_id);
    for (const auto& charge : { retrieved, page.data.front() }) {
        EXPECT_EQ(charge.customer_id, customer_id);
        EXPECT_FALSE(charge.payment_intent_id.empty());
        EXPECT_EQ(charge.payment_intent_id, retrieved.payment_intent_id);
        EXPECT_EQ(charge.status, "succeeded");
        EXPECT_TRUE(charge.paid);
        EXPECT_TRUE(charge.disputed);
        EXPECT_FALSE(charge.livemode);
        EXPECT_EQ(charge.amount, 1500);
        EXPECT_EQ(charge.amount_refunded, 0);
    }
}

TEST_F(SandboxPaymentsTest, PendingAndInitiallySuccessfulRefundsReachTheirFinalStatuses) {
    auto& client = environment->stripe_client();
    const auto pending_charge = create_charge("pm_card_pendingRefund");
    const auto failing_charge = create_charge("pm_card_refundFail");
    const auto pending =
        client.refunds()
            .create_async({ .charge_id = pending_charge,
                            .amount = 300,
                            .metadata = { { "operation", environment->run_id() + "_pending" } } },
                          { .idempotency_key = environment->run_id() + "_pending" })
            .result();
    const auto failing =
        client.refunds()
            .create_async({ .charge_id = failing_charge,
                            .amount = 400,
                            .metadata = { { "operation", environment->run_id() + "_failing" } } },
                          { .idempotency_key = environment->run_id() + "_failing" })
            .result();
    EXPECT_EQ(pending.status, "pending");
    EXPECT_EQ(failing.status, "succeeded");

    bool succeeded = false;
    bool failed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(7);
    while ((!succeeded || !failed) && std::chrono::steady_clock::now() < deadline) {
        for (const auto& original : { pending, failing }) {
            const auto page =
                client.refunds().list_async({ .charge_id = original.charge_id }).result();
            ASSERT_EQ(page.data.size(), 1U);
            EXPECT_FALSE(page.has_more);
            const auto& refund = page.data.front();
            EXPECT_EQ(refund.id, original.id);
            EXPECT_EQ(refund.charge_id, original.charge_id);
            EXPECT_EQ(refund.payment_intent_id, original.payment_intent_id);
            EXPECT_EQ(refund.amount, original.amount);
            EXPECT_EQ(refund.currency, "usd");
            EXPECT_EQ(refund.metadata, original.metadata);
            if (original.id == pending.id) {
                EXPECT_TRUE(refund.status == "pending" || refund.status == "succeeded");
                succeeded = refund.status == "succeeded";
            } else {
                EXPECT_TRUE(refund.status == "succeeded" || refund.status == "failed");
                failed = refund.status == "failed";
                if (failed) EXPECT_FALSE(refund.failure_reason.empty());
            }
        }
        if (!succeeded || !failed) std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    if (!succeeded || !failed)
        FAIL() << "Stripe refund fixtures did not reach succeeded/failed within seven minutes"
               << " (pending completed: " << succeeded << ", failure observed: " << failed << ")";
    EXPECT_EQ(client.charges().retrieve_async(pending_charge).result().amount_refunded, 300);
    EXPECT_EQ(client.charges().retrieve_async(failing_charge).result().amount_refunded, 0);
}

TEST_F(SandboxPaymentsTest, RefundCreationRetryHistoryAndRemainingBalance) {
    const auto charge_id = create_charge();
    auto& client = environment->stripe_client();
    const std::string operation = environment->run_id() + "_partial_refund";
    const CreateRefundRequest request{ .charge_id = charge_id,
                                       .amount = 300,
                                       .metadata = { { "operation", operation } } };
    const RequestOptions options{ .idempotency_key = operation,
                                  .timeout = std::chrono::seconds(30) };
    const auto first = client.refunds().create_async(request, options).result();
    EXPECT_EQ(first.charge_id, charge_id);
    EXPECT_EQ(first.amount, 300);
    EXPECT_EQ(first.currency, "usd");
    EXPECT_EQ(first.status, "succeeded");
    EXPECT_EQ(first.metadata.at("operation"), operation);
    EXPECT_FALSE(first.payment_intent_id.empty());

    std::promise<CallbackResult<Refund>> completion;
    auto result = completion.get_future();
    client.refunds().create(request, options, [&](CallbackResult<Refund> value) {
        completion.set_value(std::move(value));
    });
    const auto retried = result.get();
    if (std::holds_alternative<std::exception_ptr>(retried))
        std::rethrow_exception(std::get<std::exception_ptr>(retried));
    EXPECT_EQ(std::get<Refund>(retried).id, first.id);
    EXPECT_EQ(client.charges().retrieve_async(charge_id).result().amount_refunded, 300);

    const auto second =
        client.refunds()
            .create_async({ .charge_id = charge_id,
                            .amount = 200,
                            .metadata = { { "operation", operation + "_second" } } },
                          { .idempotency_key = operation + "_second" })
            .result();
    EXPECT_NE(second.id, first.id);
    EXPECT_EQ(second.amount, 200);
    EXPECT_EQ(second.status, "succeeded");
    EXPECT_EQ(client.charges().retrieve_async(charge_id).result().amount_refunded, 500);

    ListRefundsRequest filter{ .charge_id = charge_id, .limit = 1 };
    auto page = client.refunds().list_async(filter).result();
    ASSERT_EQ(page.data.size(), 1U);
    EXPECT_TRUE(page.has_more);
    std::set<std::string> ids{ page.data.front().id };
    EXPECT_EQ(page.data.front().charge_id, charge_id);
    filter.starting_after = page.data.front().id;
    std::promise<CallbackResult<RefundList>> page_completion;
    auto page_result = page_completion.get_future();
    client.refunds().list(filter, {}, [&](CallbackResult<RefundList> value) {
        page_completion.set_value(std::move(value));
    });
    const auto callback_page = page_result.get();
    if (std::holds_alternative<std::exception_ptr>(callback_page))
        std::rethrow_exception(std::get<std::exception_ptr>(callback_page));
    page = std::get<RefundList>(callback_page);
    ASSERT_EQ(page.data.size(), 1U);
    EXPECT_FALSE(page.has_more);
    EXPECT_TRUE(ids.insert(page.data.front().id).second);
    EXPECT_TRUE(ids.contains(first.id));
    EXPECT_TRUE(ids.contains(second.id));
    EXPECT_EQ(page.data.front().charge_id, charge_id);
    // Recover the durable operation from history without sending another create request.
    const auto history =
        client.refunds().list_async({ .charge_id = charge_id, .limit = 100 }).result();
    ASSERT_EQ(history.data.size(), 2U);
    int recovered = 0;
    for (const auto& refund : history.data) {
        if (refund.metadata.at("operation") == operation) {
            ++recovered;
            EXPECT_EQ(refund.id, first.id);
            EXPECT_EQ(refund.amount, request.amount.value());
        }
    }
    EXPECT_EQ(recovered, 1);
    filter.starting_after = page.data.front().id;
    EXPECT_TRUE(client.refunds().list_async(filter).result().data.empty());

    EXPECT_THROW(client.refunds().create_async({ .charge_id = charge_id, .amount = 1001 }).result(),
                 StripeApiException);
    const auto remaining = client.refunds()
                               .create_async({ .charge_id = charge_id },
                                             { .idempotency_key = operation + "_remaining" })
                               .result();
    EXPECT_EQ(remaining.amount, 1000);
    EXPECT_EQ(remaining.status, "succeeded");
    EXPECT_EQ(client.charges().retrieve_async(charge_id).result().amount_refunded, 1500);
    EXPECT_THROW(client.refunds().create_async({ .charge_id = charge_id, .amount = 1 }).result(),
                 StripeApiException);
    EXPECT_EQ(client.refunds().list_async({ .charge_id = charge_id }).result().data.size(), 3U);
}

TEST_F(SandboxPaymentsTest, ChargeRetrievalAndCustomerFilteredPagination) {
    const auto first = create_charge();
    const auto second = create_charge();
    auto& charges = environment->stripe_client().charges();
    const RequestOptions options{ .timeout = std::chrono::seconds(30) };
    const auto charge = charges.retrieve_async(first, options).result();
    EXPECT_EQ(charge.id, first);
    EXPECT_EQ(charge.customer_id, customer_id);
    EXPECT_FALSE(charge.payment_intent_id.empty());
    EXPECT_EQ(charge.amount, 1500);
    EXPECT_EQ(charge.amount_refunded, 0);
    EXPECT_EQ(charge.currency, "usd");
    EXPECT_EQ(charge.status, "succeeded");
    EXPECT_TRUE(charge.paid);
    EXPECT_FALSE(charge.disputed);
    EXPECT_FALSE(charge.livemode);

    std::promise<CallbackResult<Charge>> completion;
    auto result = completion.get_future();
    charges.retrieve(second, options, [&](CallbackResult<Charge> value) {
        completion.set_value(std::move(value));
    });
    const auto callback = result.get();
    if (std::holds_alternative<std::exception_ptr>(callback))
        std::rethrow_exception(std::get<std::exception_ptr>(callback));
    EXPECT_EQ(std::get<Charge>(callback).id, second);

    ListChargesRequest request{ .customer_id = customer_id, .limit = 1 };
    auto page = charges.list_async(request, options).result();
    ASSERT_EQ(page.data.size(), 1U);
    EXPECT_TRUE(page.has_more);
    std::set<std::string> ids{ page.data.front().id };
    EXPECT_EQ(page.data.front().customer_id, customer_id);
    request.starting_after = page.data.front().id;
    std::promise<CallbackResult<ChargeList>> page_completion;
    auto page_result = page_completion.get_future();
    charges.list(request, options, [&](CallbackResult<ChargeList> value) {
        page_completion.set_value(std::move(value));
    });
    const auto callback_page = page_result.get();
    if (std::holds_alternative<std::exception_ptr>(callback_page))
        std::rethrow_exception(std::get<std::exception_ptr>(callback_page));
    page = std::get<ChargeList>(callback_page);
    ASSERT_EQ(page.data.size(), 1U);
    EXPECT_FALSE(page.has_more);
    EXPECT_EQ(page.data.front().customer_id, customer_id);
    EXPECT_TRUE(ids.insert(page.data.front().id).second);
    EXPECT_TRUE(ids.contains(first));
    EXPECT_TRUE(ids.contains(second));
    request.starting_after = page.data.front().id;
    EXPECT_TRUE(charges.list_async(request).result().data.empty());
    EXPECT_THROW(charges.retrieve_async("ch_stripekit_missing_" + environment->run_id()).result(),
                 StripeApiException);
}

TEST_F(SandboxPaymentsTest, InvoiceAllocationsLinkPaymentsAndSupportCursors) {
    const auto first = pay_invoice();
    const auto second = pay_invoice();
    auto& client = environment->stripe_client();
    std::vector<std::string> allocation_ids;
    const RequestOptions modern{ .headers = { { "Stripe-Version", "2026-10-28.endive" } } };
    for (const auto& invoice : { first, second }) {
        const auto intent = invoice_payment_intent(*environment, invoice.id());
        const auto paid_intent = environment->stripe_api().get("/v1/payment_intents/" + intent);
        ASSERT_TRUE(paid_intent.ok());
        const auto charge_id = paid_intent.body["latest_charge"].asString();
        ASSERT_FALSE(intent.empty());
        ASSERT_FALSE(charge_id.empty());
        const auto charge = client.charges().retrieve_async(charge_id, modern).result();
        EXPECT_EQ(charge.customer_id, customer_id);
        EXPECT_EQ(charge.payment_intent_id, intent);
        const ListInvoicePaymentsRequest request{ .payment_intent_id = intent, .limit = 1 };
        const auto page = client.invoice_payments().list_async(request, modern).result();
        ASSERT_EQ(page.data.size(), 1U);
        EXPECT_FALSE(page.has_more);
        const auto& allocation = page.data.front();
        EXPECT_EQ(allocation.invoice_id, invoice.id());
        EXPECT_EQ(allocation.payment_type, "payment_intent");
        EXPECT_EQ(allocation.payment_intent_id, intent);
        EXPECT_EQ(allocation.status, "paid");
        EXPECT_EQ(allocation.amount_requested, 1500);
        ASSERT_TRUE(allocation.amount_paid.has_value());
        EXPECT_EQ(allocation.amount_paid.value(), 1500);
        EXPECT_EQ(allocation.currency, "usd");
        EXPECT_TRUE(allocation.is_default);
        EXPECT_FALSE(allocation.livemode);
        allocation_ids.push_back(allocation.id);
        auto next = request;
        next.starting_after = allocation.id;
        EXPECT_TRUE(client.invoice_payments().list_async(next, modern).result().data.empty());
    }
    // Both allocations exist in the unfiltered list, so the first page must have a successor.
    const auto first_page = client.invoice_payments().list_async({ .limit = 1 }, modern).result();
    ASSERT_EQ(first_page.data.size(), 1U);
    EXPECT_TRUE(first_page.has_more);
    std::promise<CallbackResult<InvoicePaymentList>> completion;
    auto result = completion.get_future();
    // Page from this test's newest allocation, not an unrelated record another run can delete.
    client.invoice_payments().list(
        { .limit = 1, .starting_after = allocation_ids.back() },
        modern,
        [&](CallbackResult<InvoicePaymentList> value) { completion.set_value(std::move(value)); });
    const auto callback = result.get();
    if (std::holds_alternative<std::exception_ptr>(callback))
        std::rethrow_exception(std::get<std::exception_ptr>(callback));
    const auto next_page = std::get<InvoicePaymentList>(callback);
    ASSERT_EQ(next_page.data.size(), 1U);
    EXPECT_NE(next_page.data.front().id, allocation_ids.back());
    EXPECT_NE(allocation_ids[0], allocation_ids[1]);

    const auto standalone = client.charges().retrieve_async(create_charge()).result();
    EXPECT_TRUE(client.invoice_payments()
                    .list_async({ .payment_intent_id = standalone.payment_intent_id }, modern)
                    .result()
                    .data.empty());
}
