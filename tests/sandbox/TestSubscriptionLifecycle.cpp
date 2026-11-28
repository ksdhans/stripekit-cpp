/** TestSubscriptionLifecycle.cpp
 *
 * Sandbox end-to-end tests for the subscription billing lifecycle and the
 * invoice signals host applications use to make entitlement decisions.
 */

#include "SandboxEnvironment.h"
#include "TestFramework.h"

#include "StripeKit/Client.h"
#include "StripeKit/Models.h"

#include <chrono>
#include <stdexcept>
#include <string>

using namespace stripekit;
using namespace stripekit::testing;

namespace {

/**
 * Returns a recurring price shared by every test in this file.
 *
 * Products and prices are per-sandbox, so the suite creates its own instead of
 * depending on data that happens to exist in the account.
 */
const std::string& shared_monthly_price() {
    static const std::string price_id = create_monthly_price(SandboxEnvironment::get(), 1500);
    return price_id;
}

} // namespace

class SandboxSubscriptionTest : public ::testing::Test {
protected:
    void SetUp() override {
        environment = &SandboxEnvironment::get();
    }

    /** Cancels a trial subscription created by this test, even after an assertion fails. */
    void TearDown() override {
        if (subscription_to_cleanup.empty()) return;

        try {
            const auto canceled = environment->stripe_client()
                                      .subscriptions()
                                      .cancel_async(subscription_to_cleanup)
                                      .result();
            if (canceled.status != "canceled")
                throw std::runtime_error("Stripe did not cancel test subscription " +
                                         subscription_to_cleanup);
        } catch (const std::exception& error) {
            throw std::runtime_error("Sandbox subscription cleanup failed: " +
                                     std::string(error.what()));
        }
    }

    SandboxEnvironment* environment = nullptr;
    std::string subscription_to_cleanup;
};

TEST_F(SandboxSubscriptionTest, CheckoutSessionIsCreatedForSubscriptionMode) {
    const std::string customer_id =
        create_customer_with_card(*environment, "pm_card_visa", "checkout");

    const CreateCheckoutSessionRequest request{ .customer_id = customer_id,
                                                .price_id = shared_monthly_price(),
                                                .success_url = "https://example.com/success",
                                                .cancel_url = "https://example.com/cancel",
                                                .metadata = { { "stripekit_run_id",
                                                                environment->run_id() } } };

    const CheckoutSession session =
        environment->stripe_client().checkout().sessions().create_async(request).result();

    EXPECT_FALSE(session.id.empty());
    EXPECT_EQ(session.customer_id, customer_id);
    EXPECT_EQ(session.mode, "subscription");
    EXPECT_EQ(session.status, "open");
    EXPECT_FALSE(session.url.empty());
    EXPECT_FALSE(session.livemode);
    EXPECT_EQ(session.metadata.at("stripekit_run_id"), environment->run_id());
}

TEST_F(SandboxSubscriptionTest, CheckoutSessionIsCreatedForPaymentMode) {
    const std::string customer_id =
        create_customer_with_card(*environment, "pm_card_visa", "checkout_payment");

    const CreateCheckoutSessionRequest request{ .customer_id = customer_id,
                                                .mode = "payment",
                                                .product_name = "One-Time Product",
                                                .currency = "usd",
                                                .unit_amount = 2500,
                                                .success_url = "https://example.com/success",
                                                .cancel_url = "https://example.com/cancel",
                                                .metadata = { { "stripekit_run_id",
                                                                environment->run_id() } } };

    const CheckoutSession session =
        environment->stripe_client().checkout().sessions().create_async(request).result();

    EXPECT_FALSE(session.id.empty());
    EXPECT_EQ(session.customer_id, customer_id);
    EXPECT_EQ(session.mode, "payment");
    EXPECT_EQ(session.status, "open");
    EXPECT_FALSE(session.url.empty());
    EXPECT_FALSE(session.livemode);
    EXPECT_EQ(session.metadata.at("stripekit_run_id"), environment->run_id());
}

TEST_F(SandboxSubscriptionTest, PaidSubscriptionEmitsInvoicePaid) {
    const std::string customer_id = create_customer_with_card(*environment, "pm_card_visa", "paid");

    const StripeApiResult created = environment->stripe_api().require_post(
        "/v1/subscriptions",
        { { "customer", customer_id },
          { "items[0][price]", shared_monthly_price() },
          { "payment_behavior", "allow_incomplete" },
          { "metadata[stripekit_run_id]", environment->run_id() } });

    const std::string subscription_id = created.id();
    EXPECT_FALSE(subscription_id.empty());

    const Subscription subscription =
        environment->stripe_client().subscriptions().retrieve_async(subscription_id).result();
    EXPECT_EQ(subscription.customer_id, customer_id);
    EXPECT_EQ(subscription.status, "active");
    EXPECT_FALSE(subscription.livemode);

    const auto event = environment->wait_for_event("invoice.paid", customer_id);
    if (!event) {
        FAIL() << "Stripe did not deliver invoice.paid for customer " << customer_id;
    }

    const Json::Value payload = event->json();
    EXPECT_EQ(json_path(payload, "data.object.customer").asString(), customer_id);
    EXPECT_EQ(json_path(payload, "data.object.status").asString(), "paid");
    EXPECT_FALSE(json_path(payload, "livemode").asBool());

    const Subscription canceled =
        environment->stripe_client().subscriptions().cancel_async(subscription_id).result();
    EXPECT_EQ(canceled.status, "canceled");
    // Stripe reports when the cancellation was requested, and the reason when it has one.
    // Neither field is required, but a cancel must not lose the reported request time.
    ASSERT_TRUE(canceled.canceled_at_unix.has_value());
    EXPECT_GE(*canceled.canceled_at_unix, 1);
    if (canceled.cancellation_details.has_value())
        EXPECT_FALSE(canceled.cancellation_details->reason.empty());
}

TEST_F(SandboxSubscriptionTest, TrialSubscriptionRetrievalIncludesTrialAndItemPeriodDates) {
    const std::string customer_id =
        create_customer_with_card(*environment, "pm_card_visa", "trial_periods");
    const StripeApiResult created = environment->stripe_api().require_post(
        "/v1/subscriptions",
        { { "customer", customer_id },
          { "items[0][price]", shared_monthly_price() },
          { "trial_period_days", "7" },
          { "metadata[stripekit_run_id]", environment->run_id() } });
    subscription_to_cleanup = created.id();
    ASSERT_FALSE(subscription_to_cleanup.empty());

    const auto subscription = environment->stripe_client()
                                  .subscriptions()
                                  .retrieve_async(subscription_to_cleanup)
                                  .result();
    EXPECT_EQ(subscription.id, subscription_to_cleanup);
    EXPECT_EQ(subscription.customer_id, customer_id);
    EXPECT_EQ(subscription.status, "trialing");
    ASSERT_TRUE(subscription.trial_end_unix.has_value());
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    EXPECT_TRUE(*subscription.trial_end_unix > now);
    ASSERT_EQ(subscription.items.size(), 1U);
    ASSERT_TRUE(subscription.items.front().current_period_start_unix.has_value());
    ASSERT_TRUE(subscription.items.front().current_period_end_unix.has_value());
    EXPECT_TRUE(*subscription.items.front().current_period_start_unix > 0);
    EXPECT_TRUE(*subscription.items.front().current_period_end_unix >
                *subscription.items.front().current_period_start_unix);
    EXPECT_EQ(subscription.items.front().price_id, shared_monthly_price());
    EXPECT_FALSE(subscription.livemode);
}

TEST_F(SandboxSubscriptionTest, DeclinedCardEmitsInvoicePaymentFailed) {
    const StripeApiResult customer = environment->stripe_api().require_post(
        "/v1/customers",
        { { "email", sandbox_email(*environment, "declined") },
          { "name", "StripeKit E2E declined" },
          { "source", "tok_chargeCustomerFail" },
          { "metadata[stripekit_run_id]", environment->run_id() } });
    const std::string customer_id = customer.id();

    const StripeApiResult created = environment->stripe_api().require_post(
        "/v1/subscriptions",
        { { "customer", customer_id },
          { "items[0][price]", shared_monthly_price() },
          { "metadata[stripekit_run_id]", environment->run_id() } });

    EXPECT_EQ(created.body["status"].asString(), "incomplete");

    const auto event = environment->wait_for_event("invoice.payment_failed", customer_id);
    if (!event) {
        FAIL() << "Stripe did not deliver invoice.payment_failed for customer " << customer_id;
    }

    const Json::Value payload = event->json();
    EXPECT_EQ(json_path(payload, "data.object.customer").asString(), customer_id);
    EXPECT_FALSE(json_path(payload, "data.object.paid").asBool());
    EXPECT_FALSE(json_path(payload, "livemode").asBool());
}
