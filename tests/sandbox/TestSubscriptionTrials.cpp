/** TestSubscriptionTrials.cpp
 *
 * See SandboxEnvironment.h for details.
 */

#include "SandboxEnvironment.h"
#include "TestFramework.h"

#include "StripeKit/Errors.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace stripekit;
using namespace stripekit::testing;

class SandboxTrialTest : public ::testing::Test {
protected:
    /** Creates a recurring price belonging only to this test. */
    void SetUp() override {
        environment = &SandboxEnvironment::get();
        const auto product =
            environment->stripe_client()
                .products()
                .create_async({ .name = "StripeKit trial " + environment->run_id() })
                .result();
        product_id = product.id;
        const auto price = environment->stripe_client()
                               .prices()
                               .create_async({ .product_id = product_id,
                                               .currency = "usd",
                                               .unit_amount = 1500,
                                               .recurring_interval = "month" })
                               .result();
        price_id = price.id;
    }

    /** Expires open sessions, deletes clock customers, and archives the test price and product. */
    void TearDown() override {
        std::string failures;
        const auto cleanup = [&failures](const std::string& id, auto operation) {
            try {
                operation();
            } catch (const std::exception& error) {
                failures += "\n" + id + ": " + error.what();
            }
        };
        for (const auto& id : sessions) {
            cleanup(id, [&] {
                const auto session =
                    environment->stripe_client().checkout().sessions().retrieve_async(id).result();
                if (session.status == "open")
                    environment->stripe_api().require_post(
                        "/v1/checkout/sessions/" + id + "/expire", {});
            });
        }
        sessions.clear();
        if (!clock_id.empty()) {
            const auto id = std::exchange(clock_id, {});
            cleanup(id, [&] {
                environment->stripe_api().require_delete("/v1/test_helpers/test_clocks/" + id);
            });
        }
        if (!price_id.empty()) {
            const auto id = std::exchange(price_id, {});
            cleanup(id, [&] {
                environment->stripe_client()
                    .prices()
                    .update_async(id, { .active = false })
                    .result();
            });
        }
        if (!product_id.empty()) {
            const auto id = std::exchange(product_id, {});
            cleanup(id, [&] {
                UpdateProductRequest request;
                request.active = false;
                environment->stripe_client().products().update_async(id, request).result();
            });
        }
        if (!failures.empty()) throw std::runtime_error("Trial test cleanup failed:" + failures);
    }

    /** Builds a card-required, seven-day trial with separate session and subscription metadata. */
    CreateCheckoutSessionRequest checkout_request() const {
        CreateCheckoutSessionRequest request;
        request.price_id = price_id;
        request.success_url = "https://example.com/success?session_id={CHECKOUT_SESSION_ID}";
        request.cancel_url = "https://example.com/cancel";
        request.trial_period_days = 7;
        request.payment_method_collection = "always";
        request.payment_method_types = { "card" };
        request.metadata = { { "session_only", environment->run_id() } };
        request.subscription_metadata = { { "app_user_id", environment->run_id() } };
        request.expires_at = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count() +
                             3600;
        return request;
    }

    /** Creates a session and records it for expiry even when a later assertion fails. */
    CheckoutSession create_session(const CreateCheckoutSessionRequest& request,
                                   const RequestOptions& options = {}) {
        const auto session = environment->stripe_client()
                                 .checkout()
                                 .sessions()
                                 .create_async(request, options)
                                 .result();
        sessions.push_back(session.id);
        return session;
    }

    /** Reads a raw Stripe object, failing explicitly on an HTTP error. */
    Json::Value get_object(const std::string& path) const {
        const auto result = environment->stripe_api().get(path);
        if (!result.ok()) throw std::runtime_error(result.describe());
        return result.body;
    }

    /** Creates a clock-bound trial with a successful default card and verifies its zero invoice. */
    Subscription create_trial() {
        frozen_time = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
        clock_id = create_sandbox_clock(*environment, frozen_time);
        const auto customer = environment->stripe_api().require_post(
            "/v1/customers",
            { { "test_clock", clock_id },
              { "email", sandbox_email(*environment, "trial_clock") },
              { "payment_method", "pm_card_visa" },
              { "invoice_settings[default_payment_method]", "pm_card_visa" } });
        const auto created = environment->stripe_api().require_post(
            "/v1/subscriptions",
            { { "customer", customer.id() },
              { "items[0][price]", price_id },
              { "trial_period_days", "7" },
              { "metadata[app_user_id]", environment->run_id() } });
        const auto subscription =
            environment->stripe_client().subscriptions().retrieve_async(created.id()).result();
        EXPECT_EQ(subscription.status, "trialing");
        EXPECT_EQ(subscription.trial_end_unix.value(), frozen_time + 7 * 86400);
        EXPECT_EQ(subscription.metadata.at("app_user_id"), environment->run_id());
        EXPECT_FALSE(subscription.livemode);
        initial_invoice_id = subscription.latest_invoice_id;
        const auto invoice =
            environment->stripe_client().invoices().retrieve_async(initial_invoice_id).result();
        EXPECT_EQ(invoice.amount_due, 0);
        EXPECT_EQ(invoice.amount_paid, 0);
        EXPECT_TRUE(invoice.paid);
        check_event("invoice.paid", initial_invoice_id);

        for (const auto& options :
             { RequestOptions{ .headers = { { "Stripe-Version", "2024-06-20" } } },
               RequestOptions{} }) {
            const auto saved = environment->stripe_client()
                                   .subscriptions()
                                   .retrieve_async(subscription.id, options)
                                   .result();
            EXPECT_EQ(saved.trial_end_unix, subscription.trial_end_unix);
            ASSERT_EQ(saved.items.size(), 1U);
            EXPECT_EQ(saved.items.front().price_id, price_id);
            if (saved.items.front().current_period_end_unix.has_value()) {
                EXPECT_EQ(saved.items.front().current_period_end_unix.value(),
                          subscription.trial_end_unix.value());
                EXPECT_EQ(saved.items.front().current_period_start_unix.value(), frozen_time);
            } else {
                EXPECT_EQ(saved.current_period_end_unix, subscription.trial_end_unix.value());
                EXPECT_EQ(saved.current_period_start_unix, frozen_time);
            }
        }
        return subscription;
    }

    /** Requires a signed event for this exact Stripe object, not just for its customer. */
    void check_event(const std::string& type, const std::string& object_id) const {
        const auto event = environment->wait_for_event(type, object_id);
        ASSERT_TRUE(event.has_value());
        EXPECT_EQ(json_path(event->json(), "data.object.id").asString(), object_id);
        EXPECT_FALSE(json_path(event->json(), "livemode").asBool());
        EXPECT_EQ(environment->dispatch_count(event->id), 1);
    }

    /** Advances through the reminder, trial expiry, and automatic invoice collection. */
    Invoice bill_trial(const Subscription& subscription,
                       const std::string& method,
                       const std::string& event_type,
                       const std::string& expected_status) {
        if (method != "pm_card_visa") {
            const auto attached = environment->stripe_api().require_post(
                "/v1/payment_methods/" + method + "/attach",
                { { "customer", subscription.customer_id } });
            environment->stripe_api().require_post("/v1/subscriptions/" + subscription.id,
                                                   { { "default_payment_method", attached.id() } });
        }
        const auto trial_end = subscription.trial_end_unix.value();
        advance_sandbox_clock(*environment, clock_id, trial_end - 3 * 86400);
        check_event("customer.subscription.trial_will_end", subscription.id);
        advance_sandbox_clock(*environment, clock_id, trial_end);
        advance_sandbox_clock(*environment, clock_id, trial_end + 3600);
        const auto saved =
            environment->stripe_client().subscriptions().retrieve_async(subscription.id).result();
        EXPECT_EQ(saved.status, expected_status);
        EXPECT_NE(saved.latest_invoice_id, initial_invoice_id);
        check_event(event_type, saved.latest_invoice_id);
        const auto invoice = environment->stripe_client()
                                 .invoices()
                                 .retrieve_async(saved.latest_invoice_id)
                                 .result();
        EXPECT_EQ(invoice.subscription_id, subscription.id);
        EXPECT_EQ(invoice.customer_id, subscription.customer_id);
        EXPECT_EQ(invoice.amount_due, 1500);
        EXPECT_EQ(invoice.billing_reason, "subscription_cycle");
        EXPECT_TRUE(invoice.created_unix >= trial_end);
        EXPECT_FALSE(invoice.livemode);
        const auto raw = get_object("/v1/invoices/" + invoice.id);
        ASSERT_FALSE(raw["lines"]["data"].empty());
        EXPECT_EQ(raw["lines"]["data"][0]["period"]["start"].asInt64(), trial_end);
        EXPECT_TRUE(raw["lines"]["data"][0]["period"]["end"].asInt64() > trial_end);
        return invoice;
    }

    SandboxEnvironment* environment = nullptr;
    std::string product_id;
    std::string price_id;
    std::string clock_id;
    std::string initial_invoice_id;
    std::int64_t frozen_time = 0;
    std::vector<std::string> sessions;
};

TEST_F(SandboxTrialTest, CheckoutTrialSettingsRetrievalAndIdempotentRetry) {
    for (const bool existing_customer : { false, true }) {
        auto request = checkout_request();
        if (existing_customer) {
            request.customer_id =
                create_customer_with_card(*environment, "pm_card_visa", "trial_checkout");
        } else {
            request.customer_email = sandbox_email(*environment, "trial_checkout_new");
        }
        const RequestOptions options{ .idempotency_key =
                                          environment->run_id() +
                                          (existing_customer ? "_trial_existing" : "_trial_new"),
                                      .headers = { { "Stripe-Version", "2024-06-20" } } };
        const auto session = create_session(request, options);
        const auto retry = environment->stripe_client()
                               .checkout()
                               .sessions()
                               .create_async(request, options)
                               .result();
        EXPECT_EQ(retry.id, session.id);
        EXPECT_EQ(retry.url, session.url);
        const auto saved = environment->stripe_client()
                               .checkout()
                               .sessions()
                               .retrieve_async(session.id, options)
                               .result();
        EXPECT_EQ(saved.status, "open");
        EXPECT_EQ(saved.mode, "subscription");
        EXPECT_EQ(saved.expires_at_unix, request.expires_at.value());
        EXPECT_EQ(saved.customer_id, request.customer_id);
        EXPECT_TRUE(saved.subscription_id.empty());
        EXPECT_EQ(saved.metadata.at("session_only"), environment->run_id());
        const auto raw = get_object("/v1/checkout/sessions/" + session.id);
        EXPECT_EQ(raw["payment_method_collection"].asString(), "always");
        ASSERT_EQ(raw["payment_method_types"].size(), 1U);
        EXPECT_EQ(raw["payment_method_types"][0].asString(), "card");
        if (!existing_customer) EXPECT_EQ(raw["customer_email"].asString(), request.customer_email);
        // Stripe exposes subscription_data only when the subscription is created.
        // The browser walkthrough checks trial duration and subscription metadata.
    }
}

TEST_F(SandboxTrialTest, AbandonedExpiredCheckoutCanStartANewAttempt) {
    auto request = checkout_request();
    request.customer_id = create_customer_with_card(*environment, "pm_card_visa", "trial_expired");
    const RequestOptions options{ .idempotency_key = environment->run_id() + "_trial_expire" };
    const auto session = create_session(request, options);
    EXPECT_TRUE(session.subscription_id.empty());
    EXPECT_EQ(session.status, "open");
    environment->stripe_api().require_post("/v1/checkout/sessions/" + session.id + "/expire", {});
    check_event("checkout.session.expired", session.id);
    const auto expired =
        environment->stripe_client().checkout().sessions().retrieve_async(session.id).result();
    EXPECT_EQ(expired.status, "expired");
    EXPECT_TRUE(expired.subscription_id.empty());
    const auto retry =
        environment->stripe_client().checkout().sessions().create_async(request, options).result();
    EXPECT_EQ(retry.id, session.id);
    EXPECT_EQ(
        environment->stripe_client().checkout().sessions().retrieve_async(retry.id).result().status,
        "expired");
    const auto replacement = create_session(
        request, { .idempotency_key = environment->run_id() + "_trial_replacement" });
    EXPECT_NE(replacement.id, expired.id);
    EXPECT_EQ(replacement.status, "open");
    const auto subscriptions = get_object("/v1/subscriptions?customer=" + request.customer_id);
    EXPECT_TRUE(subscriptions["data"].empty());
}

TEST_F(SandboxTrialTest, TrialEndCollectsTheFirstNonzeroInvoice) {
    const auto subscription = create_trial();
    const auto invoice = bill_trial(subscription, "pm_card_visa", "invoice.paid", "active");
    EXPECT_EQ(invoice.status, "paid");
    EXPECT_EQ(invoice.amount_paid, 1500);
    EXPECT_EQ(invoice.amount_remaining, 0);
    EXPECT_TRUE(invoice.paid);
}

TEST_F(SandboxTrialTest, TrialEndDeclineCanRecoverWithANewPaymentMethod) {
    const auto subscription = create_trial();
    const auto invoice = bill_trial(
        subscription, "pm_card_chargeCustomerFail", "invoice.payment_failed", "past_due");
    EXPECT_EQ(invoice.status, "open");
    EXPECT_EQ(invoice.amount_paid, 0);
    EXPECT_EQ(invoice.amount_remaining, 1500);
    EXPECT_FALSE(invoice.paid);
    EXPECT_FALSE(invoice.hosted_invoice_url.empty());
    const auto attached = environment->stripe_api().require_post(
        "/v1/payment_methods/pm_card_visa/attach", { { "customer", subscription.customer_id } });
    environment->stripe_api().require_post("/v1/subscriptions/" + subscription.id,
                                           { { "default_payment_method", attached.id() } });
    environment->stripe_api().require_post("/v1/invoices/" + invoice.id + "/pay",
                                           { { "payment_method", attached.id() } });
    check_event("invoice.paid", invoice.id);
    const auto recovered =
        environment->stripe_client().subscriptions().retrieve_async(subscription.id).result();
    EXPECT_EQ(recovered.status, "active");
    EXPECT_EQ(recovered.latest_invoice_id, invoice.id);
    EXPECT_TRUE(environment->stripe_client().invoices().retrieve_async(invoice.id).result().paid);
}

TEST_F(SandboxTrialTest, TrialEndAuthenticationNeedsCustomerAction) {
    const auto subscription = create_trial();
    const auto invoice = bill_trial(subscription,
                                    "pm_card_authenticationRequired",
                                    "invoice.payment_action_required",
                                    "past_due");
    EXPECT_EQ(invoice.status, "open");
    EXPECT_FALSE(invoice.paid);
    EXPECT_EQ(invoice.amount_remaining, 1500);
    EXPECT_FALSE(invoice.hosted_invoice_url.empty());
    const auto raw = get_object("/v1/invoices/" + invoice.id);
    const auto intent = get_object("/v1/payment_intents/" + raw["payment_intent"].asString());
    EXPECT_EQ(intent["status"].asString(), "requires_action");
    EXPECT_FALSE(intent["next_action"].isNull());
}

TEST_F(SandboxTrialTest, CancelingDuringTrialPreventsTheFirstCharge) {
    const auto subscription = create_trial();
    const auto canceled =
        environment->stripe_client().subscriptions().cancel_async(subscription.id).result();
    EXPECT_EQ(canceled.status, "canceled");
    check_event("customer.subscription.deleted", subscription.id);
    advance_sandbox_clock(*environment, clock_id, subscription.trial_end_unix.value() + 3600);
    const auto saved =
        environment->stripe_client().subscriptions().retrieve_async(subscription.id).result();
    EXPECT_EQ(saved.status, "canceled");
    EXPECT_EQ(saved.latest_invoice_id, initial_invoice_id);
    const auto invoices = get_object("/v1/invoices?subscription=" + subscription.id);
    ASSERT_EQ(invoices["data"].size(), 1U);
    EXPECT_EQ(invoices["data"][0]["id"].asString(), initial_invoice_id);
    EXPECT_EQ(invoices["data"][0]["amount_paid"].asInt64(), 0);
}

TEST_F(SandboxTrialTest, ScheduledTrialCancellationDoesNotStartPaidBilling) {
    const auto subscription = create_trial();
    environment->stripe_api().require_post("/v1/subscriptions/" + subscription.id,
                                           { { "cancel_at_period_end", "true" } });
    const auto scheduled =
        environment->stripe_client().subscriptions().retrieve_async(subscription.id).result();
    EXPECT_EQ(scheduled.status, "trialing");
    EXPECT_TRUE(scheduled.cancel_at_period_end);
    advance_sandbox_clock(*environment, clock_id, subscription.trial_end_unix.value() + 3600);
    check_event("customer.subscription.deleted", subscription.id);
    const auto saved =
        environment->stripe_client().subscriptions().retrieve_async(subscription.id).result();
    EXPECT_EQ(saved.status, "canceled");
    const auto invoices = get_object("/v1/invoices?subscription=" + subscription.id);
    ASSERT_EQ(invoices["data"].size(), 1U);
    EXPECT_EQ(invoices["data"][0]["id"].asString(), initial_invoice_id);
    EXPECT_EQ(invoices["data"][0]["amount_paid"].asInt64(), 0);
}
