/** TestSubscriptionLifecycle.cpp
 *
 * Sandbox end-to-end tests for the subscription billing lifecycle and the
 * invoice signals host applications use to make entitlement decisions.
 */

#include "SandboxEnvironment.h"
#include "TestFramework.h"

#include "StripeKit/Client.h"
#include "StripeKit/Models.h"

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
    static const std::string price_id =
        create_monthly_price(SandboxEnvironment::get(), 1500);
    return price_id;
}

} // namespace

class SandboxSubscriptionTest : public ::testing::Test {
protected:
    void SetUp() override {
        environment = &SandboxEnvironment::get();
    }

    SandboxEnvironment* environment = nullptr;
};

TEST_F(SandboxSubscriptionTest, CheckoutSessionIsCreatedForSubscriptionMode) {
    const std::string customer_id =
        create_customer_with_card(*environment, "pm_card_visa", "checkout");

    const CreateCheckoutSessionRequest request{
        .customer_id = customer_id,
        .price_id = shared_monthly_price(),
        .success_url = "https://example.com/success",
        .cancel_url = "https://example.com/cancel",
        .metadata = {{"stripekit_run_id", environment->run_id()}}
    };

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

    const CreateCheckoutSessionRequest request{
        .customer_id = customer_id,
        .mode = "payment",
        .product_name = "One-Time Product",
        .currency = "usd",
        .unit_amount = 2500,
        .success_url = "https://example.com/success",
        .cancel_url = "https://example.com/cancel",
        .metadata = {{"stripekit_run_id", environment->run_id()}}
    };

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
    const std::string customer_id =
        create_customer_with_card(*environment, "pm_card_visa", "paid");

    const StripeApiResult created = environment->stripe_api().require_post("/v1/subscriptions", {
        {"customer", customer_id},
        {"items[0][price]", shared_monthly_price()},
        {"payment_behavior", "allow_incomplete"},
        {"metadata[stripekit_run_id]", environment->run_id()}
    });

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
}

TEST_F(SandboxSubscriptionTest, DeclinedCardEmitsInvoicePaymentFailed) {
    const StripeApiResult customer = environment->stripe_api().require_post("/v1/customers", {
        {"email", sandbox_email(*environment, "declined")},
        {"name", "StripeKit E2E declined"},
        {"source", "tok_chargeCustomerFail"},
        {"metadata[stripekit_run_id]", environment->run_id()}
    });
    const std::string customer_id = customer.id();

    const StripeApiResult created = environment->stripe_api().require_post("/v1/subscriptions", {
        {"customer", customer_id},
        {"items[0][price]", shared_monthly_price()},
        {"metadata[stripekit_run_id]", environment->run_id()}
    });

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
