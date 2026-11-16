/** TestCollectionAndInvoices.cpp
 *
 * See SandboxEnvironment.h for details.
 */
#include "SandboxEnvironment.h"
#include "TestFramework.h"
#include "StripeKit/Errors.h"

#include <future>
#include <set>

using namespace stripekit;
using namespace stripekit::testing;

namespace {
/** Wait for a callback result and rethrow its failure without disguising it as a value. */
template <typename T>
T completed(std::future<CallbackResult<T>>& result) {
    auto value = result.get();
    if (std::holds_alternative<std::exception_ptr>(value))
        std::rethrow_exception(std::get<std::exception_ptr>(value));
    return std::get<T>(std::move(value));
}
} // namespace

class SandboxCollectionAndInvoicesTest : public ::testing::Test {
protected:
    /** Create an isolated clock, customer and paid subscription for each scenario. */
    void SetUp() override {
        environment = &SandboxEnvironment::get();
        frozen = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
        clock = create_sandbox_clock(*environment, frozen);
        customer =
            environment->stripe_api()
                .require_post("/v1/customers",
                              { { "test_clock", clock },
                                { "payment_method", "pm_card_visa" },
                                { "invoice_settings[default_payment_method]", "pm_card_visa" } })
                .id();
        product = environment->stripe_client()
                      .products()
                      .create_async({ .name = "Collection tests " + environment->run_id() })
                      .result()
                      .id;
        price = environment->stripe_client()
                    .prices()
                    .create_async({ .product_id = product,
                                    .currency = "usd",
                                    .unit_amount = 1500,
                                    .recurring_interval = "month" })
                    .result()
                    .id;
        subscription = environment->stripe_api()
                           .require_post("/v1/subscriptions",
                                         { { "customer", customer }, { "items[0][price]", price } })
                           .id();
        const auto saved =
            environment->stripe_client().subscriptions().retrieve_async(subscription).result();
        if (saved.status != "active")
            throw std::runtime_error("Collection fixture subscription is not active");
        paid_invoice = saved.latest_invoice_id;
        renewal =
            saved.items.front().current_period_end_unix.value_or(saved.current_period_end_unix);
        if (renewal <= frozen) throw std::runtime_error("Collection fixture has no future renewal");
    }

    /** Delete clock-bound billing objects and archive catalog objects, reporting cleanup errors. */
    void TearDown() override {
        std::string failures;
        const auto cleanup = [&](const std::string& id, auto action) {
            if (id.empty()) return;
            try {
                action();
            } catch (const std::exception& error) {
                failures += "\n" + id + ": " + error.what();
            }
        };
        cleanup(clock, [&] {
            environment->stripe_api().require_delete("/v1/test_helpers/test_clocks/" + clock);
        });
        cleanup(price, [&] {
            environment->stripe_client().prices().update_async(price, { .active = false }).result();
        });
        cleanup(product, [&] {
            environment->stripe_client()
                .products()
                .update_async(product, { .active = false })
                .result();
        });
        if (!failures.empty())
            throw std::runtime_error("Collection test cleanup failed:" + failures);
    }

    /** Create a non-advancing draft linked to this subscription with a nonzero amount. */
    std::string draft_invoice(bool linked = true) {
        FormParams fields{ { "customer", customer }, { "auto_advance", "false" } };
        if (linked) fields.emplace_back("subscription", subscription);
        else fields.emplace_back("currency", "usd");
        const auto invoice = environment->stripe_api().require_post("/v1/invoices", fields).id();
        environment->stripe_api().require_post("/v1/invoiceitems",
                                               { { "customer", customer },
                                                 { "invoice", invoice },
                                                 { "currency", "usd" },
                                                 { "amount", "500" } });
        return invoice;
    }

    /** Verify a retrieved subscription still belongs to this scenario's customer. */
    Subscription saved_subscription() {
        auto saved =
            environment->stripe_client().subscriptions().retrieve_async(subscription).result();
        EXPECT_EQ(saved.id, subscription);
        EXPECT_EQ(saved.customer_id, customer);
        return saved;
    }

    SandboxEnvironment* environment = nullptr;
    std::string clock, customer, product, price, subscription, paid_invoice;
    std::int64_t frozen = 0, renewal = 0;
};

// Withdrawing a period-end cancellation should allow billing to continue.
// Scheduling it again should end the subscription at renewal without changing
// the invoice that already paid for the current period.
TEST_F(SandboxCollectionAndInvoicesTest, PeriodEndCancellationCanBeClearedAndOccursAtRenewal) {
    auto& api = environment->stripe_client().subscriptions();
    // Scheduling and retrying cancellation must leave the already-paid period active.
    const RequestOptions options{ .idempotency_key = environment->run_id() + "_cancel_period" };
    auto changed = api.set_cancel_at_period_end_async(subscription, true, options).result();
    EXPECT_TRUE(changed.cancel_at_period_end);
    EXPECT_EQ(changed.status, "active");
    EXPECT_TRUE(saved_subscription().cancel_at_period_end);
    EXPECT_TRUE(api.set_cancel_at_period_end_async(subscription, true, options)
                    .result()
                    .cancel_at_period_end);
    // Withdraw the cancellation, then schedule it again for the same renewal boundary.
    std::promise<CallbackResult<Subscription>> completion;
    auto result = completion.get_future();
    api.set_cancel_at_period_end(subscription, false, {}, [&](CallbackResult<Subscription> value) {
        completion.set_value(std::move(value));
    });
    EXPECT_FALSE(completed(result).cancel_at_period_end);
    EXPECT_FALSE(saved_subscription().cancel_at_period_end);
    api.set_cancel_at_period_end_async(subscription, true).result();
    advance_sandbox_clock(*environment, clock, renewal + 1);
    EXPECT_EQ(saved_subscription().status, "canceled");
    EXPECT_TRUE(environment->stripe_client().invoices().retrieve_async(paid_invoice).result().paid);
}

// Clearing a cancellation leaves the subscription active past the original date.
// Rescheduling cancels it at the new date, without another renewal invoice, and
// delivers the cancellation webhook. Application access rules are not tested here.
TEST_F(SandboxCollectionAndInvoicesTest, DatedCancellationCanBeClearedAndRescheduled) {
    auto& client = environment->stripe_client();
    auto& api = client.subscriptions();
    const auto first_date = frozen + 7 * 86400;
    const auto final_date = frozen + 14 * 86400;
    ASSERT_TRUE(final_date < renewal);
    // Schedule a mid-period cancellation without crediting back unused time.
    UpdateSubscriptionRequest request;
    request.cancel_at = first_date;
    request.proration_behavior = ProrationBehavior::None;
    request.metadata = { { "keep", "yes" }, { "remove", "yes" } };
    const RequestOptions options{ .idempotency_key = subscription + "_dated_cancel" };
    const auto changed = api.update_async(subscription, request, options).result();
    EXPECT_EQ(changed.cancel_at_unix.value(), first_date);
    EXPECT_EQ(changed.status, "active");
    EXPECT_FALSE(changed.livemode);
    EXPECT_EQ(changed.latest_invoice_id, paid_invoice);
    EXPECT_EQ(saved_subscription().cancel_at_unix.value(), first_date);
    EXPECT_EQ(api.update_async(subscription, request, options).result().cancel_at_unix.value(),
              first_date);

    // Withdrawing cancellation must preserve unrelated subscription metadata.
    UpdateSubscriptionRequest clear;
    clear.cancel_at = SubscriptionCancelAt::Clear;
    clear.proration_behavior = ProrationBehavior::None;
    clear.metadata = { { "remove", "" } };
    std::promise<CallbackResult<Subscription>> completion;
    auto result = completion.get_future();
    api.update(subscription, clear, {}, [&](CallbackResult<Subscription> value) {
        completion.set_value(std::move(value));
    });
    const auto continued = completed(result);
    EXPECT_FALSE(continued.cancel_at_unix.has_value());
    EXPECT_FALSE(continued.cancel_at_period_end);
    auto saved = saved_subscription();
    EXPECT_FALSE(saved.cancel_at_unix.has_value());
    EXPECT_EQ(saved.metadata.at("keep"), "yes");
    EXPECT_EQ(saved.metadata.count("remove"), 0U);
    // Passing the withdrawn deadline must not end the subscription.
    advance_sandbox_clock(*environment, clock, first_date + 1);
    EXPECT_EQ(saved_subscription().status, "active");

    // The subscription stays active until the new cancellation date.
    request.cancel_at = final_date;
    api.update_async(subscription, request).result();
    advance_sandbox_clock(*environment, clock, final_date - 1);
    EXPECT_EQ(saved_subscription().status, "active");
    advance_sandbox_clock(*environment, clock, final_date + 1);
    saved = saved_subscription();
    EXPECT_EQ(saved.status, "canceled");
    EXPECT_EQ(saved.latest_invoice_id, paid_invoice);
    // Passing the original renewal date must not generate another invoice.
    advance_sandbox_clock(*environment, clock, renewal + 3600);
    EXPECT_EQ(saved_subscription().status, "canceled");
    const auto invoices =
        client.invoices().list_async({ .subscription_id = subscription, .limit = 10 }).result();
    EXPECT_FALSE(invoices.has_more);
    ASSERT_EQ(invoices.data.size(), 1U);
    EXPECT_EQ(invoices.data.front().id, paid_invoice);
    EXPECT_EQ(invoices.data.front().amount_paid, 1500);
    EXPECT_TRUE(invoices.data.front().paid);
    // The application learns about the completed cancellation through its webhook.
    const auto event = environment->wait_for_event("customer.subscription.deleted", subscription);
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(json_path(event->json(), "data.object.status").asString(), "canceled");
}

// Shortening a paid period can credit the unused half. Check that callers can
// suppress that credit, leave it for a later invoice, or invoice it immediately,
// without creating duplicate adjustments.
TEST_F(SandboxCollectionAndInvoicesTest, DatedCancellationHonorsProrationPolicies) {
    auto& api = environment->stripe_client().subscriptions();
    // Cancel halfway through a 1500-cent period: any unused-service credit should be 750 cents.
    const auto cancel_date = frozen + (renewal - frozen) / 2;
    for (const auto policy : { ProrationBehavior::None,
                               ProrationBehavior::CreateProrations,
                               ProrationBehavior::AlwaysInvoice }) {
        const auto id =
            policy == ProrationBehavior::None
                ? subscription
                : environment->stripe_api()
                      .require_post("/v1/subscriptions",
                                    { { "customer", customer }, { "items[0][price]", price } })
                      .id();
        const auto before = api.retrieve_async(id).result();
        UpdateSubscriptionRequest request;
        request.cancel_at = cancel_date;
        request.proration_behavior = policy;
        const auto changed = api.update_async(id, request).result();
        EXPECT_EQ(changed.cancel_at_unix.value(), cancel_date);
        EXPECT_EQ(changed.status, "active");
        if (policy == ProrationBehavior::AlwaysInvoice) {
            // Immediate invoicing must put the credit on a new invoice now.
            EXPECT_NE(changed.latest_invoice_id, before.latest_invoice_id);
            const auto invoice = environment->stripe_client()
                                     .invoices()
                                     .retrieve_async(changed.latest_invoice_id)
                                     .result();
            EXPECT_EQ(invoice.total, -750);
        } else {
            EXPECT_EQ(changed.latest_invoice_id, before.latest_invoice_id);
        }
        // Collect remaining adjustments: only the deferred policy should still have a credit.
        const auto pending = environment->stripe_api().require_post(
            "/v1/invoices",
            { { "customer", customer }, { "subscription", id }, { "auto_advance", "false" } });
        EXPECT_EQ(pending.body["total"].asInt64(),
                  policy == ProrationBehavior::CreateProrations ? -750 : 0);
    }
}

TEST_F(SandboxCollectionAndInvoicesTest, CollectionPauseBehaviorsControlRenewalInvoicesAndResume) {
    auto& client = environment->stripe_client();
    const auto existing_open = draft_invoice();
    environment->stripe_api().require_post("/v1/invoices/" + existing_open + "/finalize",
                                           { { "auto_advance", "false" } });
    for (const auto& [behavior, name, status] :
         std::vector<std::tuple<CollectionPauseBehavior, std::string, std::string>>{
             { CollectionPauseBehavior::Void, "void", "void" },
             { CollectionPauseBehavior::KeepAsDraft, "keep_as_draft", "draft" },
             { CollectionPauseBehavior::MarkUncollectible,
               "mark_uncollectible",
               "uncollectible" } }) {
        const auto key = subscription + "_pause_" + name;
        const auto paused =
            client.subscriptions()
                .pause_collection_async(subscription, { behavior, {} }, { .idempotency_key = key })
                .result();
        ASSERT_TRUE(paused.pause_collection.has_value());
        EXPECT_EQ(paused.pause_collection->behavior, name);
        EXPECT_FALSE(paused.pause_collection->resumes_at_unix.has_value());
        EXPECT_EQ(paused.status, "active");
        EXPECT_EQ(saved_subscription().pause_collection->behavior, name);
        EXPECT_TRUE(client.invoices().retrieve_async(paid_invoice).result().paid);
        EXPECT_EQ(client.invoices().retrieve_async(existing_open).result().status, "open");
        advance_sandbox_clock(*environment, clock, renewal + 3600);
        auto saved = saved_subscription();
        const auto invoice_id = saved.latest_invoice_id;
        EXPECT_NE(invoice_id, paid_invoice);
        const auto invoice = client.invoices().retrieve_async(invoice_id).result();
        EXPECT_EQ(invoice.subscription_id, subscription);
        EXPECT_EQ(invoice.customer_id, customer);
        EXPECT_EQ(invoice.status, status);
        EXPECT_EQ(invoice.amount_paid, 0);
        std::promise<CallbackResult<Subscription>> completion;
        auto result = completion.get_future();
        client.subscriptions().resume_collection(
            subscription, {}, [&](CallbackResult<Subscription> value) {
                completion.set_value(std::move(value));
            });
        EXPECT_FALSE(completed(result).pause_collection.has_value());
        EXPECT_FALSE(saved_subscription().pause_collection.has_value());
        EXPECT_EQ(client.invoices().retrieve_async(invoice_id).result().status, status);
        if (status == "draft") {
            EXPECT_THROW(client.invoices().delete_async(invoice_id).result(), StripeApiException);
            const auto finalized = client.invoices().finalize_async(invoice_id).result();
            EXPECT_EQ(finalized.status, "open");
            EXPECT_FALSE(finalized.auto_advance.value());
            EXPECT_EQ(finalized.amount_paid, 0);
            EXPECT_EQ(client.invoices().void_async(invoice_id).result().status, "void");
        }
        renewal =
            saved.items.front().current_period_end_unix.value_or(saved.current_period_end_unix);
    }
    const auto resume_at = renewal + 86400;
    std::promise<CallbackResult<Subscription>> completion;
    auto result = completion.get_future();
    client.subscriptions().pause_collection(
        subscription,
        { CollectionPauseBehavior::Void, resume_at },
        {},
        [&](CallbackResult<Subscription> value) { completion.set_value(std::move(value)); });
    EXPECT_EQ(completed(result).pause_collection->resumes_at_unix.value(), resume_at);
    EXPECT_EQ(saved_subscription().pause_collection->resumes_at_unix.value(), resume_at);
    advance_sandbox_clock(*environment, clock, resume_at + 1);
    EXPECT_FALSE(saved_subscription().pause_collection.has_value());
    const auto following = saved_subscription();
    const auto following_renewal =
        following.items.front().current_period_end_unix.value_or(following.current_period_end_unix);
    advance_sandbox_clock(*environment, clock, following_renewal + 3600);
    const auto collected =
        client.invoices().retrieve_async(saved_subscription().latest_invoice_id).result();
    EXPECT_EQ(collected.status, "paid");
    EXPECT_EQ(collected.amount_paid, 1500);
    client.subscriptions().pause_collection_async(subscription, {}).result();
    client.subscriptions().resume_collection_async(subscription).result();
    EXPECT_FALSE(saved_subscription().pause_collection.has_value());
}

TEST_F(SandboxCollectionAndInvoicesTest, InvoicePaginationVoidingDeletionAndPaidInvoiceProtection) {
    auto& invoices = environment->stripe_client().invoices();
    const auto draft = draft_invoice();
    const auto open = draft_invoice();
    const auto unrelated = draft_invoice(false);
    std::promise<CallbackResult<Invoice>> finalize_completion;
    auto finalize_result = finalize_completion.get_future();
    invoices.finalize(open, {}, [&](CallbackResult<Invoice> value) {
        finalize_completion.set_value(std::move(value));
    });
    EXPECT_EQ(completed(finalize_result).status, "open");
    EXPECT_FALSE(invoices.retrieve_async(open).result().auto_advance.value());
    ListInvoicesRequest filter{ .subscription_id = subscription, .limit = 1 };
    std::set<std::string> seen;
    auto page = invoices.list_async(filter).result();
    for (int pages = 0; pages < 5; ++pages) {
        ASSERT_EQ(page.data.size(), 1U);
        const auto& invoice = page.data.front();
        EXPECT_EQ(invoice.customer_id, customer);
        EXPECT_EQ(invoice.subscription_id, subscription);
        ASSERT_TRUE(seen.insert(invoice.id).second);
        EXPECT_NE(invoice.id, unrelated);
        if (!page.has_more) break;
        filter.starting_after = invoice.id;
        std::promise<CallbackResult<InvoiceList>> completion;
        auto result = completion.get_future();
        invoices.list(filter, {}, [&](CallbackResult<InvoiceList> value) {
            completion.set_value(std::move(value));
        });
        page = completed(result);
    }
    EXPECT_FALSE(page.has_more);
    EXPECT_EQ(seen.size(), 3U);
    EXPECT_TRUE(seen.contains(draft));
    EXPECT_TRUE(seen.contains(open));
    EXPECT_TRUE(seen.contains(paid_invoice));
    filter.starting_after = page.data.back().id;
    EXPECT_TRUE(invoices.list_async(filter).result().data.empty());
    const RequestOptions options{ .idempotency_key = open + "_void" };
    EXPECT_EQ(invoices.void_async(open, options).result().status, "void");
    EXPECT_EQ(invoices.retrieve_async(open).result().status, "void");
    std::promise<CallbackResult<Invoice>> void_completion;
    auto void_result = void_completion.get_future();
    invoices.void_invoice(open, options, [&](CallbackResult<Invoice> value) {
        void_completion.set_value(std::move(value));
    });
    EXPECT_EQ(completed(void_result).status, "void");
    EXPECT_THROW(invoices.delete_async(draft).result(), StripeApiException);
    const auto finalized = invoices.finalize_async(draft).result();
    EXPECT_EQ(finalized.status, "open");
    EXPECT_FALSE(finalized.auto_advance.value());
    EXPECT_EQ(finalized.amount_paid, 0);
    EXPECT_EQ(invoices.void_async(draft).result().status, "void");
    EXPECT_THROW(invoices.void_async(unrelated).result(), StripeApiException);
    std::promise<CallbackResult<DeletedInvoice>> delete_completion;
    auto delete_result = delete_completion.get_future();
    invoices.delete_invoice(unrelated, {}, [&](CallbackResult<DeletedInvoice> value) {
        delete_completion.set_value(std::move(value));
    });
    const auto deleted = completed(delete_result);
    EXPECT_EQ(deleted.id, unrelated);
    EXPECT_TRUE(deleted.deleted);
    EXPECT_THROW(invoices.retrieve_async(unrelated).result(), StripeApiException);
    EXPECT_THROW(invoices.delete_async(paid_invoice).result(), StripeApiException);
    EXPECT_THROW(invoices.void_async(paid_invoice).result(), StripeApiException);
    EXPECT_THROW(invoices.delete_async(open).result(), StripeApiException);
    const auto another_draft = draft_invoice(false);
    EXPECT_TRUE(invoices.delete_async(another_draft).result().deleted);
    const auto paid = invoices.retrieve_async(paid_invoice).result();
    EXPECT_EQ(paid.status, "paid");
    EXPECT_TRUE(paid.paid);
    EXPECT_EQ(paid.amount_paid, 1500);
}
