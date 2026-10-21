/** TestBillingPortalAndInvoices.cpp
 *
 * Sandbox tests for reading invoices and for billing portal sessions and
 * configurations.
 */

#include "SandboxEnvironment.h"
#include "TestFramework.h"

#include "StripeKit/Client.h"
#include "StripeKit/Errors.h"
#include "StripeKit/Models.h"

#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace stripekit;
using namespace stripekit::testing;

class SandboxBillingPortalAndInvoicesTest : public ::testing::Test {
protected:
    /** Uses the sandbox client and account selected for this run. */
    void SetUp() override {
        environment = &SandboxEnvironment::get();
    }

    /**
     * Cancels test subscriptions, deactivates portal configurations, and
     * archives catalog objects. Cleanup failures fail the test, after
     * attempting the remaining cleanup.
     */
    void TearDown() override {
        std::string failures;
        const auto cleanup = [&failures](const std::string& id, auto operation) {
            try {
                operation();
            } catch (const std::exception& error) {
                failures += "\n" + id + ": " + error.what();
            }
        };
        for (const auto& id : subscriptions) {
            cleanup(id, [&] {
                environment->stripe_client().subscriptions().cancel_async(id).result();
            });
        }
        // Stripe cannot delete portal configurations, so deactivate them instead.
        for (const auto& id : portal_configurations) {
            cleanup(id, [&] {
                environment->stripe_client()
                    .billing_portal()
                    .configurations()
                    .update_async(id, { .active = false })
                    .result();
            });
        }
        for (const auto& id : prices) {
            cleanup(id, [&] {
                environment->stripe_client()
                    .prices()
                    .update_async(id, { .active = false })
                    .result();
            });
        }
        for (const auto& id : products) {
            cleanup(id, [&] {
                UpdateProductRequest changes;
                changes.active = false;
                environment->stripe_client().products().update_async(id, changes).result();
            });
        }
        subscriptions.clear();
        portal_configurations.clear();
        prices.clear();
        products.clear();
        if (!failures.empty()) {
            throw std::runtime_error("Sandbox billing portal cleanup failed:" + failures);
        }
    }

    /** Creates a run-specific product and records it for archiving. */
    Product create_product(const std::string& label) {
        CreateProductRequest request;
        request.id = "stripekit_" + environment->run_id() + "_" + label;
        request.name = "StripeKit E2E " + label;
        request.description = "Sandbox billing portal and invoice test";
        request.metadata = { { "stripekit_run_id", environment->run_id() } };
        const auto product = environment->stripe_client().products().create_async(request).result();
        products.push_back(product.id);
        return product;
    }

    /** Creates a recurring USD price and records it for archiving. */
    Price create_price(const Product& product,
                       const std::string& label,
                       std::int64_t amount = 1500,
                       const std::string& interval = "month") {
        CreatePriceRequest request;
        request.product_id = product.id;
        request.currency = "usd";
        request.unit_amount = amount;
        request.recurring_interval = interval;
        request.lookup_key = environment->run_id() + "_" + label;
        request.metadata = { { "stripekit_run_id", environment->run_id() } };
        const auto price = environment->stripe_client().prices().create_async(request).result();
        prices.push_back(price.id);
        return price;
    }

    /**
     * Subscribes a new customer paying with the given Stripe test payment
     * method, and records the subscription for cancellation.
     *
     * The subscription is created even when its first payment fails or needs
     * authentication, leaving its first invoice open.
     */
    Subscription create_subscription(const Price& price,
                                     const std::string& test_payment_method,
                                     const std::string& label) {
        const auto customer_id =
            create_customer_with_card(*environment, test_payment_method, label);
        const auto created = environment->stripe_api().require_post(
            "/v1/subscriptions",
            { { "customer", customer_id },
              { "items[0][price]", price.id },
              { "payment_behavior", "allow_incomplete" },
              { "metadata[stripekit_run_id]", environment->run_id() } });
        subscriptions.push_back(created.id());
        const auto subscription =
            environment->stripe_client().subscriptions().retrieve_async(created.id()).result();
        EXPECT_EQ(subscription.items.size(), 1U);
        EXPECT_FALSE(subscription.latest_invoice_id.empty());
        return subscription;
    }

    /**
     * Returns the API versions invoices are read with: the account's default,
     * which uses the current invoice layout, and an older version that keeps
     * the legacy `paid` and `subscription` fields.
     */
    static std::vector<std::string> invoice_api_versions() {
        return { "", "2024-06-20" };
    }

    /** Fetches an invoice through StripeKit, optionally pinned to an API version. */
    Invoice retrieve_invoice(const std::string& invoice_id, const std::string& api_version) {
        RequestOptions options;
        if (!api_version.empty()) options.headers = { { "Stripe-Version", api_version } };
        return environment->stripe_client().invoices().retrieve_async(invoice_id, options).result();
    }

    /** Portal features offering plan changes between the given prices of one product. */
    static BillingPortalFeatures portal_features(const Product& product,
                                                 const std::vector<Price>& plan_prices) {
        BillingPortalFeatures features;
        features.customer_update = { .enabled = true, .allowed_updates = { "email", "address" } };
        features.subscription_cancel = { .enabled = true,
                                         .mode = "at_period_end",
                                         .proration_behavior = "" };
        BillingPortalProduct plans{ .product_id = product.id, .price_ids = {} };
        for (const auto& price : plan_prices)
            plans.price_ids.push_back(price.id);
        features.subscription_update = { .enabled = true,
                                         .default_allowed_updates = { "price" },
                                         .products = { plans },
                                         .proration_behavior = "create_prorations" };
        return features;
    }

    /**
     * Makes sure the sandbox has a default portal configuration.
     *
     * Stripe makes the first configuration created in an account its default,
     * and a default configuration cannot be deactivated. In a new sandbox this
     * creates a small configuration that stays as the account's default, so the
     * configurations made by the tests are never the default.
     */
    void ensure_default_portal_configuration() {
        static bool checked = false;
        if (checked) return;
        auto& api = environment->stripe_api();
        const auto existing = api.get("/v1/billing_portal/configurations?is_default=true&limit=1");
        if (!existing.ok()) {
            throw std::runtime_error("Stripe rejected the default portal configuration lookup - " +
                                     existing.describe());
        }
        if (existing.body["data"].empty()) {
            api.require_post("/v1/billing_portal/configurations",
                             { { "name", "StripeKit sandbox default" },
                               { "features[invoice_history][enabled]", "true" },
                               { "features[payment_method_update][enabled]", "true" },
                               { "default_return_url", "https://example.com/billing" } });
        }
        checked = true;
    }

    /** Creates a run-specific portal configuration and records it for deactivation. */
    BillingPortalConfiguration create_portal_configuration(const Product& product,
                                                           const std::vector<Price>& plan_prices) {
        ensure_default_portal_configuration();
        const auto configuration =
            environment->stripe_client()
                .billing_portal()
                .configurations()
                .create_async({ .features = portal_features(product, plan_prices),
                                .name = "StripeKit E2E " + product.id,
                                .headline = "StripeKit sandbox plans",
                                .default_return_url = "https://example.com/billing",
                                .metadata = { { "stripekit_run_id", environment->run_id() } } })
                .result();
        portal_configurations.push_back(configuration.id);
        return configuration;
    }

    /** Checks that Stripe stored the features built by portal_features(). */
    static void check_portal_features(const BillingPortalFeatures& features,
                                      const Product& product,
                                      const std::vector<Price>& plan_prices) {
        EXPECT_TRUE(features.invoice_history_enabled);
        EXPECT_TRUE(features.payment_method_update_enabled);
        EXPECT_TRUE(features.customer_update.enabled);
        EXPECT_EQ(std::set<std::string>(features.customer_update.allowed_updates.begin(),
                                        features.customer_update.allowed_updates.end()),
                  (std::set<std::string>{ "email", "address" }));
        EXPECT_TRUE(features.subscription_cancel.enabled);
        EXPECT_EQ(features.subscription_cancel.mode, "at_period_end");
        EXPECT_TRUE(features.subscription_update.enabled);
        EXPECT_EQ(features.subscription_update.default_allowed_updates,
                  std::vector<std::string>{ "price" });
        EXPECT_EQ(features.subscription_update.proration_behavior, "create_prorations");
        ASSERT_EQ(features.subscription_update.products.size(), 1U);
        const auto& plans = features.subscription_update.products.front();
        EXPECT_EQ(plans.product_id, product.id);
        std::set<std::string> expected;
        for (const auto& price : plan_prices)
            expected.insert(price.id);
        EXPECT_EQ(std::set<std::string>(plans.price_ids.begin(), plans.price_ids.end()), expected);
    }

    SandboxEnvironment* environment = nullptr;
    std::vector<std::string> products;
    std::vector<std::string> prices;
    std::vector<std::string> subscriptions;
    std::vector<std::string> portal_configurations;
};

TEST_F(SandboxBillingPortalAndInvoicesTest, DeclinedInvoiceReportsWhatIsOwedAndLaterPaid) {
    const auto product = create_product("invoice_declined");
    const auto price = create_price(product, "invoice_declined_monthly");
    const auto subscription =
        create_subscription(price, "pm_card_chargeCustomerFail", "invoice_declined");
    const auto invoice_id = subscription.latest_invoice_id;
    for (const auto& version : invoice_api_versions()) {
        const auto open = retrieve_invoice(invoice_id, version);
        EXPECT_EQ(open.id, invoice_id);
        EXPECT_EQ(open.customer_id, subscription.customer_id);
        EXPECT_EQ(open.subscription_id, subscription.id);
        EXPECT_EQ(open.status, "open");
        EXPECT_FALSE(open.paid);
        EXPECT_EQ(open.currency, "usd");
        EXPECT_EQ(open.amount_due, 1500);
        EXPECT_EQ(open.amount_paid, 0);
        EXPECT_EQ(open.amount_remaining, 1500);
        EXPECT_FALSE(open.billing_reason.empty());
        EXPECT_TRUE(open.attempt_count >= 1);
        EXPECT_FALSE(open.number.empty());
        EXPECT_EQ(open.hosted_invoice_url.rfind("https://", 0), 0U);
        EXPECT_TRUE(open.created_unix > 0);
        EXPECT_FALSE(open.livemode);
    }

    const auto card = environment->stripe_api().require_post(
        "/v1/payment_methods/pm_card_visa/attach", { { "customer", subscription.customer_id } });
    environment->stripe_api().require_post("/v1/invoices/" + invoice_id + "/pay",
                                           { { "payment_method", card.id() } });
    for (const auto& version : invoice_api_versions()) {
        const auto settled = retrieve_invoice(invoice_id, version);
        EXPECT_EQ(settled.status, "paid");
        EXPECT_TRUE(settled.paid);
        EXPECT_EQ(settled.subscription_id, subscription.id);
        EXPECT_EQ(settled.amount_paid, 1500);
        EXPECT_EQ(settled.amount_remaining, 0);
        // A manual payment after a decline does not always raise the attempt count.
        EXPECT_TRUE(settled.attempt_count >= 1);
        EXPECT_FALSE(settled.next_payment_attempt_unix.has_value());
        EXPECT_FALSE(settled.invoice_pdf.empty());
    }
}

TEST_F(SandboxBillingPortalAndInvoicesTest, AuthenticationRequiredInvoiceHasHostedPaymentPage) {
    const auto product = create_product("invoice_authentication");
    const auto price = create_price(product, "invoice_authentication_monthly");
    const auto subscription =
        create_subscription(price, "pm_card_authenticationRequired", "invoice_authentication");
    EXPECT_EQ(subscription.status, "incomplete");
    for (const auto& version : invoice_api_versions()) {
        const auto pending = retrieve_invoice(subscription.latest_invoice_id, version);
        EXPECT_EQ(pending.subscription_id, subscription.id);
        EXPECT_EQ(pending.status, "open");
        EXPECT_FALSE(pending.paid);
        EXPECT_EQ(pending.amount_remaining, pending.amount_due);
        EXPECT_TRUE(pending.amount_remaining > 0);
        EXPECT_EQ(pending.hosted_invoice_url.rfind("https://", 0), 0U);
    }
}

TEST_F(SandboxBillingPortalAndInvoicesTest, MissingInvoiceReportsStripeError) {
    try {
        environment->stripe_client()
            .invoices()
            .retrieve_async("in_stripekit_missing_" + environment->run_id())
            .result();
        FAIL() << "Stripe returned an invoice that should not exist";
    } catch (const StripeApiException& error) {
        EXPECT_EQ(error.payload().http_status, 404);
        EXPECT_EQ(error.payload().code, "resource_missing");
    }
}

TEST_F(SandboxBillingPortalAndInvoicesTest, PortalConfigurationRoundTripsUpdatesAndDeactivates) {
    const auto product = create_product("portal_config");
    const auto monthly = create_price(product, "portal_config_monthly");
    const auto yearly = create_price(product, "portal_config_yearly", 18000, "year");
    auto& configurations = environment->stripe_client().billing_portal().configurations();
    const auto created = create_portal_configuration(product, { monthly, yearly });
    EXPECT_TRUE(created.active);
    EXPECT_FALSE(created.is_default);
    EXPECT_FALSE(created.livemode);
    EXPECT_EQ(created.headline, "StripeKit sandbox plans");
    EXPECT_EQ(created.default_return_url, "https://example.com/billing");
    EXPECT_EQ(created.metadata.at("stripekit_run_id"), environment->run_id());
    check_portal_features(created.features, product, { monthly, yearly });

    const auto retrieved = configurations.retrieve_async(created.id).result();
    EXPECT_EQ(retrieved.id, created.id);
    EXPECT_EQ(retrieved.name, created.name);
    check_portal_features(retrieved.features, product, { monthly, yearly });

    auto features = portal_features(product, { monthly });
    features.subscription_cancel.enabled = false;
    features.customer_update = { .enabled = false, .allowed_updates = {} };
    features.invoice_history_enabled = false;
    const auto updated = configurations
                             .update_async(created.id,
                                           { .features = features,
                                             .name = created.name + " updated",
                                             .headline = std::string{} })
                             .result();
    EXPECT_EQ(updated.name, created.name + " updated");
    EXPECT_TRUE(updated.headline.empty());
    EXPECT_EQ(updated.default_return_url, "https://example.com/billing");
    EXPECT_FALSE(updated.features.subscription_cancel.enabled);
    EXPECT_FALSE(updated.features.customer_update.enabled);
    EXPECT_FALSE(updated.features.invoice_history_enabled);
    EXPECT_TRUE(updated.features.payment_method_update_enabled);
    ASSERT_EQ(updated.features.subscription_update.products.size(), 1U);
    EXPECT_EQ(updated.features.subscription_update.products.front().price_ids,
              std::vector<std::string>{ monthly.id });

    const auto inactive = configurations.update_async(created.id, { .active = false }).result();
    EXPECT_FALSE(inactive.active);
}

TEST_F(SandboxBillingPortalAndInvoicesTest, PortalSessionsOpenEachFlowForASubscription) {
    const auto product = create_product("portal_flows");
    const auto monthly = create_price(product, "portal_flows_monthly");
    const auto yearly = create_price(product, "portal_flows_yearly", 18000, "year");
    const auto subscription = create_subscription(monthly, "pm_card_visa", "portal_flows");
    EXPECT_EQ(subscription.status, "active");
    const auto configuration = create_portal_configuration(product, { monthly, yearly });
    auto& sessions = environment->stripe_client().billing_portal().sessions();
    const auto open = [&](std::optional<BillingPortalFlow> flow, const std::string& locale = "") {
        const auto session = sessions
                                 .create_async({ .customer_id = subscription.customer_id,
                                                 .return_url = "https://example.com/billing",
                                                 .configuration_id = configuration.id,
                                                 .locale = locale,
                                                 .flow = std::move(flow) })
                                 .result();
        EXPECT_EQ(session.customer_id, subscription.customer_id);
        EXPECT_EQ(session.configuration_id, configuration.id);
        EXPECT_EQ(session.return_url, "https://example.com/billing");
        EXPECT_EQ(session.url.rfind("https://", 0), 0U);
        EXPECT_FALSE(session.livemode);
        return session;
    };

    EXPECT_EQ(open(std::nullopt, "nl").locale, "nl");
    open(BillingPortalFlow{
        .type = BillingPortalFlowType::SubscriptionUpdateConfirm,
        .subscription_id = subscription.id,
        .items = { { .item_id = subscription.items.front().id, .price_id = yearly.id } },
        .after_completion = BillingPortalAfterCompletion::Redirect,
        .redirect_url = "https://example.com/billing/processing" });
    open(BillingPortalFlow{ .type = BillingPortalFlowType::PaymentMethodUpdate,
                            .after_completion = BillingPortalAfterCompletion::HostedConfirmation,
                            .confirmation_message = "Your card has been updated." });
    open(BillingPortalFlow{ .type = BillingPortalFlowType::SubscriptionCancel,
                            .subscription_id = subscription.id,
                            .after_completion = BillingPortalAfterCompletion::PortalHomepage });
    open(BillingPortalFlow{ .type = BillingPortalFlowType::SubscriptionUpdate,
                            .subscription_id = subscription.id });

    // Opening a session only prepares the change; the customer must still confirm it.
    const auto saved =
        environment->stripe_client().subscriptions().retrieve_async(subscription.id).result();
    ASSERT_EQ(saved.items.size(), 1U);
    EXPECT_EQ(saved.items.front().price_id, monthly.id);
}

TEST_F(SandboxBillingPortalAndInvoicesTest, PortalConfirmFlowRejectsPricesOutsideTheConfiguration) {
    const auto product = create_product("portal_reject");
    const auto monthly = create_price(product, "portal_reject_monthly");
    const auto unlisted = create_price(product, "portal_reject_yearly", 18000, "year");
    const auto subscription = create_subscription(monthly, "pm_card_visa", "portal_reject");
    const auto configuration = create_portal_configuration(product, { monthly });
    try {
        environment->stripe_client()
            .billing_portal()
            .sessions()
            .create_async(
                { .customer_id = subscription.customer_id,
                  .return_url = "https://example.com/billing",
                  .configuration_id = configuration.id,
                  .flow =
                      BillingPortalFlow{ .type = BillingPortalFlowType::SubscriptionUpdateConfirm,
                                         .subscription_id = subscription.id,
                                         .items = { { .item_id = subscription.items.front().id,
                                                      .price_id = unlisted.id } } } })
            .result();
        FAIL() << "Stripe accepted a plan change to a price outside the configuration";
    } catch (const StripeApiException& error) {
        EXPECT_EQ(error.payload().http_status, 400);
        EXPECT_EQ(error.payload().type, StripeErrorType::InvalidRequest);
    }
}
