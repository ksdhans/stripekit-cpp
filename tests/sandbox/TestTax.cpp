/** TestTax.cpp
 *
 * See SandboxEnvironment.h for details.
 */
#include "SandboxEnvironment.h"
#include "TestFramework.h"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace stripekit;
using namespace stripekit::testing;

namespace {

/** A fictional customer address used to exercise Stripe Tax in the sandbox. */
struct TaxTestAddress {
    std::string line1;
    std::string city;
    std::string state;
    std::string postal_code;
    std::string country;
};

/** Fictional customer and business details used by the Stripe Tax sandbox tests. */
struct TaxTestFixtures {
    std::string uk_vat_id = "GB123456789";
    TaxTestAddress new_zealand{
        .line1 = "1 Example Street", .city = "Auckland", .postal_code = "1010", .country = "NZ"
    };
    TaxTestAddress united_kingdom{
        .line1 = "1 Example Street", .city = "London", .postal_code = "SW1A 1AA", .country = "GB"
    };
    TaxTestAddress australia{ .line1 = "1 Example Street",
                              .city = "Sydney",
                              .state = "NSW",
                              .postal_code = "2000",
                              .country = "AU" };
};

/** Add an address to a Stripe form request under the supplied parameter prefix. */
void append_address(FormParams& params,
                    const TaxTestAddress& address,
                    std::string_view prefix = "address") {
    const std::string key(prefix);
    params.emplace_back(key + "[line1]", address.line1);
    params.emplace_back(key + "[city]", address.city);
    if (!address.state.empty()) params.emplace_back(key + "[state]", address.state);
    params.emplace_back(key + "[postal_code]", address.postal_code);
    params.emplace_back(key + "[country]", address.country);
}

/** Configure the fictional business and confirm Stripe Tax is ready in test mode. */
void ensure_tax_settings(SandboxEnvironment& environment, const TaxTestFixtures& fixtures) {
    auto& api = environment.stripe_api();
    const auto matches_fixtures = [&](const Json::Value& settings) {
        return settings["livemode"].isBool() && !settings["livemode"].asBool() &&
               json_path(settings, "status").asString() == "active" &&
               json_path(settings, "defaults.tax_behavior").asString() == "exclusive" &&
               json_path(settings, "defaults.tax_code").asString() == "txcd_10103001" &&
               json_path(settings, "head_office.address.line1").asString() ==
                   fixtures.new_zealand.line1 &&
               json_path(settings, "head_office.address.city").asString() ==
                   fixtures.new_zealand.city &&
               json_path(settings, "head_office.address.postal_code").asString() ==
                   fixtures.new_zealand.postal_code &&
               json_path(settings, "head_office.address.country").asString() ==
                   fixtures.new_zealand.country;
    };

    auto settings = api.get("/v1/tax/settings");
    if (!settings.ok())
        throw std::runtime_error("Could not read Stripe Tax settings: " + settings.describe());
    if (!settings.body["livemode"].isBool())
        throw std::runtime_error(
            "Stripe Tax settings response did not identify whether the account is in live mode");
    if (settings.body["livemode"].asBool())
        throw std::runtime_error(
            "Refusing to run tax tests: STRIPE_API_KEY is associated with a live account, not a "
            "test sandbox");
    if (!matches_fixtures(settings.body)) {
        FormParams params{ { "defaults[tax_behavior]", "exclusive" },
                           { "defaults[tax_code]", "txcd_10103001" } };
        append_address(params, fixtures.new_zealand, "head_office[address]");
        settings = api.require_post("/v1/tax/settings", params);
    }

    if (json_path(settings.body, "status").asString() != "active" ||
        !matches_fixtures(settings.body))
        throw std::runtime_error(
            "Stripe Tax settings could not be activated with the sandbox test fixtures");
}

/** Ensure one active test-mode registration exists for each tax-positive location. */
void ensure_tax_registrations(SandboxEnvironment& environment) {
    auto& api = environment.stripe_api();
    bool new_zealand_active = false;
    bool united_kingdom_active = false;
    bool australia_active = false;
    bool new_zealand_scheduled = false;
    bool united_kingdom_scheduled = false;
    bool australia_scheduled = false;
    std::string starting_after;

    do {
        std::string path = "/v1/tax/registrations?limit=100&status=all";
        if (!starting_after.empty()) path += "&starting_after=" + starting_after;
        const auto response = api.get(path);
        if (!response.ok())
            throw std::runtime_error("Could not list Stripe Tax registrations: " +
                                     response.describe());
        const auto& data = response.body["data"];
        const auto& has_more = response.body["has_more"];
        if (!data.isArray() || !has_more.isBool())
            throw std::runtime_error("Stripe returned a malformed tax registration list");

        for (const auto& registration : data) {
            if (!registration.isObject() || !registration["country"].isString() ||
                !registration["status"].isString())
                throw std::runtime_error("Stripe returned a malformed tax registration");
            const auto country = registration["country"].asString();
            const auto status = registration["status"].asString();
            const bool target_location = country == "NZ" || country == "GB" || country == "AU";
            if (!target_location || (status != "active" && status != "scheduled")) continue;
            if (!registration["livemode"].isBool() || registration["livemode"].asBool())
                throw std::runtime_error(
                    "Stripe returned a live-mode tax registration; refusing tax tests");

            if (country == "NZ") {
                new_zealand_active |= status == "active";
                new_zealand_scheduled |= status == "scheduled";
            } else if (country == "GB") {
                united_kingdom_active |= status == "active";
                united_kingdom_scheduled |= status == "scheduled";
            } else {
                australia_active |= status == "active";
                australia_scheduled |= status == "scheduled";
            }
        }

        if (!has_more.asBool()) break;
        if (data.empty() || !data[data.size() - 1]["id"].isString() ||
            data[data.size() - 1]["id"].asString().empty())
            throw std::runtime_error(
                "Stripe returned an invalid cursor while listing tax registrations");
        starting_after = data[data.size() - 1]["id"].asString();
    } while (true);

    if (australia_active || australia_scheduled)
        throw std::runtime_error(
            "The no-tax control location has an active or scheduled registration; remove or "
            "expire it in this sandbox before running the tax tests");
    if (new_zealand_scheduled || united_kingdom_scheduled)
        throw std::runtime_error(
            "A required tax location has a scheduled registration; resolve it in this sandbox "
            "before running the tax tests");

    const auto create_registration = [&](std::string_view country, std::string_view country_key) {
        const auto registration = api.require_post(
            "/v1/tax/registrations",
            { { "country", std::string(country) },
              { "country_options[" + std::string(country_key) + "][type]", "standard" },
              { "active_from", "now" } });
        if (!registration.body["livemode"].isBool() || registration.body["livemode"].asBool())
            throw std::runtime_error(
                "Stripe created a live-mode tax registration; refusing tax tests");
        if (json_path(registration.body, "country").asString() != country ||
            json_path(registration.body, "status").asString() != "active")
            throw std::runtime_error(
                "Stripe did not activate the required sandbox tax registration");
    };
    if (!new_zealand_active) create_registration("NZ", "nz");
    if (!united_kingdom_active) create_registration("GB", "gb");
}

/** Prepare test-mode Tax settings and registrations without changing unrelated locations. */
void ensure_tax_sandbox(SandboxEnvironment& environment, const TaxTestFixtures& fixtures) {
    ensure_tax_settings(environment, fixtures);
    ensure_tax_registrations(environment);
}

/** Track sandbox objects so the test can remove them even after an assertion fails. */
class TaxTestResources {
public:
    explicit TaxTestResources(SandboxEnvironment& environment) : environment(environment) {}

    /** Cancel subscriptions and retire objects created by this test, reporting cleanup failures. */
    void cleanup() {
        std::string failures;
        const auto cleanup = [&](const std::string& id, auto action) {
            if (id.empty()) return;
            try {
                action();
            } catch (const std::exception& error) {
                failures += "\n" + id + ": " + error.what();
            }
        };
        for (const auto& id : subscriptions)
            cleanup(id,
                    [&] { environment.stripe_api().require_delete("/v1/subscriptions/" + id); });
        for (const auto& id : invoices)
            cleanup(id, [&] {
                const auto invoice =
                    environment.stripe_client().invoices().retrieve_async(id).result();
                if (invoice.status == "draft")
                    environment.stripe_client().invoices().delete_async(id).result();
                else if (invoice.status == "open")
                    environment.stripe_client().invoices().void_async(id).result();
            });
        for (const auto& id : sessions)
            cleanup(id, [&] {
                const auto session =
                    environment.stripe_client().checkout().sessions().retrieve_async(id).result();
                if (session.status == "open")
                    environment.stripe_client().checkout().sessions().expire_async(id).result();
            });
        for (const auto& id : customers)
            cleanup(id, [&] { environment.stripe_api().require_delete("/v1/customers/" + id); });
        for (const auto& id : prices)
            cleanup(id, [&] {
                environment.stripe_client().prices().update_async(id, { .active = false }).result();
            });
        if (!product.empty())
            cleanup(product, [&] {
                environment.stripe_client()
                    .products()
                    .update_async(product, { .active = false })
                    .result();
            });
        for (const auto& id : coupons)
            cleanup(id, [&] { environment.stripe_api().require_delete("/v1/coupons/" + id); });
        for (const auto& id : clocks)
            cleanup(id, [&] {
                environment.stripe_api().require_delete("/v1/test_helpers/test_clocks/" + id);
            });
        if (!failures.empty())
            throw std::runtime_error("Stripe Tax test cleanup failed:" + failures);
    }

    /** Stripe product created for the tax test. */
    std::string product;
    /** Stripe Price IDs created for one-time and recurring purchases. */
    std::vector<std::string> prices;
    /** Stripe customer IDs created for each tax scenario. */
    std::vector<std::string> customers;
    /** Stripe subscription IDs created for each tax scenario. */
    std::vector<std::string> subscriptions;
    /** Stripe invoice IDs created for one-time invoice purchases. */
    std::vector<std::string> invoices;
    /** Checkout sessions created to verify Checkout tax request handling. */
    std::vector<std::string> sessions;
    /** Stripe test clock IDs created for renewal coverage. */
    std::vector<std::string> clocks;
    /** Stripe coupon IDs created for renewal coverage. */
    std::vector<std::string> coupons;

private:
    SandboxEnvironment& environment;
};

struct TaxTestPrices {
    std::string currency;
    std::string recurring;
    std::string one_time;
};

/** Create one product with a tax-exclusive recurring and one-time Price per currency. */
std::vector<TaxTestPrices> create_tax_prices(SandboxEnvironment& environment,
                                             TaxTestResources& resources) {
    auto& api = environment.stripe_api();
    resources.product = api.require_post("/v1/products",
                                         { { "name", "StripeKit tax test " + environment.run_id() },
                                           { "metadata[stripekit_run_id]", environment.run_id() } })
                            .id();
    std::vector<TaxTestPrices> prices;
    for (const auto& currency : { "nzd", "gbp", "aud" }) {
        const auto create_price = [&](bool recurring) {
            FormParams params{ { "product", resources.product },
                               { "currency", currency },
                               { "unit_amount", "1000" },
                               { "tax_behavior", "exclusive" },
                               { "metadata[stripekit_run_id]", environment.run_id() } };
            if (recurring) params.emplace_back("recurring[interval]", "month");
            const auto price = api.require_post("/v1/prices", params).id();
            resources.prices.push_back(price);
            return price;
        };
        prices.push_back({ .currency = currency,
                           .recurring = create_price(true),
                           .one_time = create_price(false) });
    }
    return prices;
}

/** Create a customer with a card and the requested fictional billing address. */
std::string create_tax_customer(SandboxEnvironment& environment,
                                TaxTestResources& resources,
                                const TaxTestAddress& address,
                                std::string_view label,
                                std::string_view clock = {}) {
    FormParams params{ { "email", sandbox_email(environment, label) },
                       { "name", "StripeKit Tax Test " + std::string(label) },
                       { "payment_method", "pm_card_visa" },
                       { "invoice_settings[default_payment_method]", "pm_card_visa" },
                       { "metadata[stripekit_run_id]", environment.run_id() } };
    if (!clock.empty()) params.emplace_back("test_clock", std::string(clock));
    append_address(params, address);
    const auto customer = environment.stripe_api().require_post("/v1/customers", params).id();
    resources.customers.push_back(customer);
    return customer;
}

/** Create a subscription with automatic tax and return its first invoice ID. */
std::string create_tax_subscription(SandboxEnvironment& environment,
                                    TaxTestResources& resources,
                                    const std::string& customer,
                                    const std::string& price,
                                    std::string_view coupon = {}) {
    FormParams params{ { "customer", customer },
                       { "items[0][price]", price },
                       { "automatic_tax[enabled]", "true" },
                       { "metadata[stripekit_run_id]", environment.run_id() } };
    if (!coupon.empty()) params.emplace_back("discounts[0][coupon]", std::string(coupon));
    const auto result = environment.stripe_api().require_post("/v1/subscriptions", params);
    resources.subscriptions.push_back(result.id());
    const auto invoice_id = json_path(result.body, "latest_invoice").asString();
    if (invoice_id.empty())
        throw std::runtime_error("Stripe did not return the subscription's first invoice");
    return invoice_id;
}

/** Create a tax-enabled draft invoice that callers can finalize or use for recovery tests. */
std::string create_draft_tax_invoice(SandboxEnvironment& environment,
                                     TaxTestResources& resources,
                                     const std::string& customer,
                                     const std::string& price) {
    auto& api = environment.stripe_api();
    const auto invoice =
        api.require_post("/v1/invoices",
                         { { "customer", customer },
                           { "collection_method", "charge_automatically" },
                           { "auto_advance", "false" },
                           { "automatic_tax[enabled]", "true" },
                           { "metadata[stripekit_run_id]", environment.run_id() } });
    resources.invoices.push_back(invoice.id());
    api.require_post("/v1/invoiceitems",
                     { { "customer", customer },
                       { "invoice", invoice.id() },
                       { "price", price },
                       { "quantity", "1" } });
    return invoice.id();
}

/** Create and pay a one-off invoice using Stripe Tax for the supplied Price. */
std::string create_tax_invoice(SandboxEnvironment& environment,
                               TaxTestResources& resources,
                               const std::string& customer,
                               const std::string& price) {
    const auto invoice = create_draft_tax_invoice(environment, resources, customer, price);
    environment.stripe_client().invoices().finalize_async(invoice).result();
    environment.stripe_api().require_post("/v1/invoices/" + invoice + "/pay", {});
    return invoice;
}

/** Require a tax-location API error rather than accepting an unrelated Stripe failure. */
template <typename Action>
void expect_tax_location_error(Action action) {
    bool rejected = false;
    try {
        action();
    } catch (const StripeApiException& error) {
        rejected = true;
        EXPECT_EQ(error.payload().code, "customer_tax_location_invalid");
        EXPECT_EQ(error.payload().http_status, 400);
        EXPECT_FALSE(error.payload().request_id.empty());
    }
    ASSERT_TRUE(rejected);
}

/** Save a valid taxable billing address and require Stripe to validate it immediately. */
void correct_tax_location(SandboxEnvironment& environment,
                          const std::string& customer,
                          const TaxTestAddress& address) {
    const auto corrected =
        environment.stripe_client()
            .customers()
            .update_async(customer,
                          { .address = BillingAddress{ .line1 = address.line1,
                                                       .city = address.city,
                                                       .state = address.state,
                                                       .postal_code = address.postal_code,
                                                       .country = address.country },
                            .validate_tax_location = TaxLocationValidation::Immediately })
            .result();
    EXPECT_EQ(corrected.id, customer);
}

/** Verify Stripe calculated tax and successfully paid the invoice. */
void expect_tax_result(SandboxEnvironment& environment,
                       const std::string& invoice_id,
                       bool expect_tax) {
    const auto invoice = environment.stripe_client().invoices().retrieve_async(invoice_id).result();
    ASSERT_TRUE(invoice.automatic_tax.has_value());
    EXPECT_TRUE(invoice.automatic_tax->enabled);
    EXPECT_EQ(invoice.automatic_tax->status, "complete");
    ASSERT_TRUE(invoice.amount_tax.has_value());
    if (expect_tax) EXPECT_TRUE(invoice.amount_tax.value() > 0);
    else EXPECT_EQ(invoice.amount_tax.value(), 0);
    EXPECT_TRUE(invoice.paid);
    EXPECT_EQ(invoice.status, "paid");
    ASSERT_FALSE(invoice.line_items.empty());
    const auto line_items = environment.stripe_client()
                                .invoices()
                                .list_line_items_async(invoice_id, { .limit = 1 })
                                .result();
    ASSERT_EQ(line_items.data.size(), 1U);
    EXPECT_EQ(line_items.data.front().id, invoice.line_items.front().id);
    EXPECT_EQ(line_items.data.front().price_id, invoice.line_items.front().price_id);
    EXPECT_EQ(line_items.data.front().quantity.value(), 1);
}

/** Verify a Checkout session accepts tax settings without completing hosted Checkout. */
void expect_checkout_tax_enabled(SandboxEnvironment& environment,
                                 TaxTestResources& resources,
                                 const std::string& customer,
                                 const std::string& price,
                                 std::string_view mode) {
    CreateCheckoutSessionRequest request;
    request.mode = std::string(mode);
    request.customer_id = customer;
    request.price_id = price;
    request.success_url = "https://example.com/stripekit-tax/success";
    request.cancel_url = "https://example.com/stripekit-tax/cancel";
    request.automatic_tax_enabled = true;
    request.billing_address_collection = "auto";
    request.customer_update_address = true;
    request.customer_update_name = true;
    request.tax_id_collection_enabled = true;
    const auto session =
        environment.stripe_client().checkout().sessions().create_async(request).result();
    resources.sessions.push_back(session.id);
    ASSERT_TRUE(session.automatic_tax.has_value());
    EXPECT_TRUE(session.automatic_tax->enabled);
    EXPECT_EQ(session.mode, mode);
    EXPECT_EQ(session.status, "open");
}

/** Return the first matching currency's Price pair. */
const TaxTestPrices& prices_for(const std::vector<TaxTestPrices>& prices,
                                std::string_view currency) {
    for (const auto& value : prices)
        if (value.currency == currency) return value;
    throw std::runtime_error("No tax test Price exists for currency " + std::string(currency));
}

/** Verify all four customer/tax-ID profiles through invoices and Checkout requests. */
void run_tax_profiles(SandboxEnvironment& environment,
                      TaxTestResources& resources,
                      const TaxTestFixtures& fixtures,
                      const std::vector<TaxTestPrices>& prices) {
    struct Profile {
        const char* label;
        const TaxTestAddress* address;
        const char* currency;
        bool expected_tax;
        bool add_uk_vat_id;
    };
    const std::vector<Profile> profiles{
        { "nz", &fixtures.new_zealand, "nzd", true, false },
        { "uk_no_vat_id", &fixtures.united_kingdom, "gbp", true, false },
        { "uk_with_vat_id", &fixtures.united_kingdom, "gbp", false, true },
        { "au_unregistered", &fixtures.australia, "aud", false, false }
    };

    for (const auto& profile : profiles) {
        const auto customer =
            create_tax_customer(environment, resources, *profile.address, profile.label);
        if (profile.add_uk_vat_id)
            environment.stripe_api().require_post(
                "/v1/customers/" + customer + "/tax_ids",
                { { "type", "gb_vat" }, { "value", fixtures.uk_vat_id } });
        const auto& price = prices_for(prices, profile.currency);
        expect_checkout_tax_enabled(environment, resources, customer, price.one_time, "payment");
        expect_checkout_tax_enabled(
            environment, resources, customer, price.recurring, "subscription");
        const auto first_invoice =
            create_tax_subscription(environment, resources, customer, price.recurring);
        expect_tax_result(environment, first_invoice, profile.expected_tax);
        const auto one_time_invoice =
            create_tax_invoice(environment, resources, customer, price.one_time);
        expect_tax_result(environment, one_time_invoice, profile.expected_tax);
    }
}

/** Verify a schedule update preserves automatic tax and a time-limited coupon at renewal. */
void run_tax_schedule_renewal(SandboxEnvironment& environment,
                              TaxTestResources& resources,
                              const TaxTestFixtures& fixtures,
                              const TaxTestPrices& nz_prices) {
    const auto frozen = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const auto clock = create_sandbox_clock(environment, frozen);
    resources.clocks.push_back(clock);
    const auto customer =
        create_tax_customer(environment, resources, fixtures.new_zealand, "schedule_tax", clock);
    const auto coupon = environment.stripe_api()
                            .require_post("/v1/coupons",
                                          { { "percent_off", "10" },
                                            { "duration", "repeating" },
                                            { "duration_in_months", "2" } })
                            .id();
    resources.coupons.push_back(coupon);
    const auto first_invoice =
        create_tax_subscription(environment, resources, customer, nz_prices.recurring, coupon);
    expect_tax_result(environment, first_invoice, true);

    auto& client = environment.stripe_client();
    const auto subscription =
        client.subscriptions().retrieve_async(resources.subscriptions.back()).result();
    const auto renewal = subscription.items.front().current_period_end_unix.value_or(
        subscription.current_period_end_unix);
    ASSERT_TRUE(renewal > frozen);
    auto& schedules = client.subscription_schedules();
    const auto schedule = schedules.create_async({ .from_subscription = subscription.id }).result();
    ASSERT_TRUE(schedule.current_phase.has_value());
    ASSERT_FALSE(schedule.phases.empty());
    SubscriptionSchedulePhaseRequest current;
    current.start_date = schedule.current_phase->start_date_unix;
    current.end_date = renewal;
    current.items = schedule.phases.front().items;
    current.automatic_tax_enabled = true;
    current.discounts = { { .coupon_id = coupon } };
    SubscriptionSchedulePhaseRequest future;
    future.start_date = renewal;
    future.duration = SubscriptionSchedulePhaseDuration{ "month", 2 };
    future.items = schedule.phases.front().items;
    future.automatic_tax_enabled = true;
    future.discounts = { { .coupon_id = coupon } };
    UpdateSubscriptionScheduleRequest update;
    update.default_automatic_tax_enabled = true;
    update.phases = { std::move(current), std::move(future) };
    const auto changed = schedules.update_async(schedule.id, update).result();
    ASSERT_TRUE(changed.default_automatic_tax.has_value());
    EXPECT_TRUE(changed.default_automatic_tax->enabled);
    ASSERT_EQ(changed.phases.size(), 2U);
    ASSERT_TRUE(changed.phases[1].automatic_tax.has_value());
    EXPECT_TRUE(changed.phases[1].automatic_tax->enabled);
    ASSERT_EQ(changed.phases[1].discounts.size(), 1U);
    EXPECT_EQ(changed.phases[1].discounts.front().coupon_id, coupon);

    // Allow Stripe's renewal invoice finalization and automatic payment delay.
    advance_sandbox_clock(environment, clock, renewal + 3600);
    auto renewed = client.subscriptions().retrieve_async(subscription.id).result();
    ASSERT_FALSE(renewed.latest_invoice_id.empty());
    expect_tax_result(environment, renewed.latest_invoice_id, true);
    const auto discounted_invoice =
        client.invoices().retrieve_async(renewed.latest_invoice_id).result();
    ASSERT_TRUE(discounted_invoice.amount_discount.has_value());
    EXPECT_TRUE(discounted_invoice.amount_discount.value() > 0);
    const auto next_renewal =
        renewed.items.front().current_period_end_unix.value_or(renewed.current_period_end_unix);
    ASSERT_TRUE(next_renewal > renewal);

    advance_sandbox_clock(environment, clock, next_renewal + 3600);
    renewed = client.subscriptions().retrieve_async(subscription.id).result();
    expect_tax_result(environment, renewed.latest_invoice_id, true);
    const auto expired_discount_invoice =
        client.invoices().retrieve_async(renewed.latest_invoice_id).result();
    ASSERT_TRUE(expired_discount_invoice.automatic_tax.has_value());
    EXPECT_TRUE(expired_discount_invoice.automatic_tax->enabled);
    ASSERT_TRUE(expired_discount_invoice.amount_tax.has_value());
    EXPECT_TRUE(expired_discount_invoice.amount_tax.value() > 0);
    EXPECT_EQ(expired_discount_invoice.amount_discount.value_or(0), 0);
}

} // namespace

class SandboxTaxTest : public ::testing::Test {
protected:
    /** Prepare the test-mode Tax settings before any Stripe objects are created. */
    void SetUp() override {
        environment = &SandboxEnvironment::get();
        ensure_tax_sandbox(*environment, fixtures);
    }

    /** Retire all temporary objects created by the tax scenarios. */
    void TearDown() override {
        if (resources) resources->cleanup();
        resources.reset();
    }

    SandboxEnvironment* environment = nullptr;
    TaxTestFixtures fixtures;
    std::unique_ptr<TaxTestResources> resources;
};

TEST_F(SandboxTaxTest, CheckoutInvoicesAndSubscriptionsReflectConfiguredTax) {
    resources = std::make_unique<TaxTestResources>(*environment);
    const auto prices = create_tax_prices(*environment, *resources);
    run_tax_profiles(*environment, *resources, fixtures, prices);
}

TEST_F(SandboxTaxTest, ScheduleUpdatesKeepTaxAndDiscountDurationThroughRenewal) {
    resources = std::make_unique<TaxTestResources>(*environment);
    const auto prices = create_tax_prices(*environment, *resources);
    run_tax_schedule_renewal(*environment, *resources, fixtures, prices_for(prices, "nzd"));
}

TEST_F(SandboxTaxTest, InvalidLocationBlocksFinalizationUntilCustomerAndDraftAreCorrected) {
    resources = std::make_unique<TaxTestResources>(*environment);
    const auto prices = create_tax_prices(*environment, *resources);
    const auto customer = create_customer_with_card(*environment, "pm_card_visa", "missing_tax");
    resources->customers.push_back(customer);
    auto& client = environment->stripe_client();
    expect_tax_location_error([&] {
        client.customers()
            .update_async(customer, { .validate_tax_location = TaxLocationValidation::Immediately })
            .result();
    });

    const auto invoice_id = create_draft_tax_invoice(
        *environment, *resources, customer, prices_for(prices, "nzd").one_time);
    expect_tax_location_error([&] { client.invoices().finalize_async(invoice_id).result(); });
    const auto failed = client.invoices().retrieve_async(invoice_id).result();
    EXPECT_EQ(failed.status, "draft");
    EXPECT_FALSE(failed.paid);
    ASSERT_TRUE(failed.automatic_tax.has_value());
    EXPECT_TRUE(failed.automatic_tax->enabled);
    EXPECT_EQ(failed.automatic_tax->status, "requires_location_inputs");
    EXPECT_EQ(failed.last_finalization_error_code, "customer_tax_location_invalid");

    correct_tax_location(*environment, customer, fixtures.new_zealand);
    const auto updated =
        client.invoices()
            .update_async(invoice_id, { .automatic_tax_enabled = true, .auto_advance = false })
            .result();
    EXPECT_EQ(updated.status, "draft");
    ASSERT_TRUE(updated.automatic_tax.has_value());
    EXPECT_TRUE(updated.automatic_tax->enabled);
    const auto finalized = client.invoices().finalize_async(invoice_id).result();
    EXPECT_EQ(finalized.status, "open");
    EXPECT_FALSE(finalized.paid);
    environment->stripe_api().require_post("/v1/invoices/" + invoice_id + "/pay", {});
    expect_tax_result(*environment, invoice_id, true);
    EXPECT_TRUE(
        client.invoices().retrieve_async(invoice_id).result().last_finalization_error_code.empty());
}

TEST_F(SandboxTaxTest, InvalidRenewalLocationDisablesTaxAndScheduleRecoveryRestoresIt) {
    resources = std::make_unique<TaxTestResources>(*environment);
    const auto prices = create_tax_prices(*environment, *resources);
    const auto frozen = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const auto clock = create_sandbox_clock(*environment, frozen);
    resources->clocks.push_back(clock);
    const auto customer =
        create_tax_customer(*environment, *resources, fixtures.new_zealand, "tax_recovery", clock);
    const auto first_invoice = create_tax_subscription(
        *environment, *resources, customer, prices_for(prices, "nzd").recurring);
    expect_tax_result(*environment, first_invoice, true);

    auto& client = environment->stripe_client();
    auto subscription =
        client.subscriptions().retrieve_async(resources->subscriptions.back()).result();
    const auto renewal = subscription.items.front().current_period_end_unix.value_or(
        subscription.current_period_end_unix);
    ASSERT_TRUE(renewal > frozen);
    const auto schedule = client.subscription_schedules()
                              .create_async({ .from_subscription = subscription.id })
                              .result();
    ASSERT_TRUE(schedule.current_phase.has_value());
    ASSERT_FALSE(schedule.phases.empty());
    SubscriptionSchedulePhaseRequest initial;
    initial.start_date = schedule.current_phase->start_date_unix;
    initial.duration = SubscriptionSchedulePhaseDuration{ "month", 3 };
    initial.items = schedule.phases.front().items;
    initial.automatic_tax_enabled = true;
    // Keep the schedule active through both renewals instead of releasing after the first.
    client.subscription_schedules()
        .update_async(schedule.id, { .phases = { initial }, .default_automatic_tax_enabled = true })
        .result();
    client.customers()
        .update_async(
            customer,
            { .address =
                  BillingAddress{
                      .line1 = "", .city = "", .state = "", .postal_code = "", .country = "" },
              .validate_tax_location = TaxLocationValidation::Deferred })
        .result();
    expect_tax_location_error([&] {
        client.customers()
            .update_async(customer, { .validate_tax_location = TaxLocationValidation::Immediately })
            .result();
    });

    advance_sandbox_clock(*environment, clock, renewal + 3600);
    subscription = client.subscriptions().retrieve_async(subscription.id).result();
    ASSERT_NE(subscription.latest_invoice_id, first_invoice);
    const auto untaxed = client.invoices().retrieve_async(subscription.latest_invoice_id).result();
    EXPECT_EQ(untaxed.status, "paid");
    EXPECT_TRUE(untaxed.paid);
    EXPECT_EQ(untaxed.amount_tax.value_or(0), 0);
    ASSERT_TRUE(untaxed.automatic_tax.has_value());
    EXPECT_FALSE(untaxed.automatic_tax->enabled);
    EXPECT_EQ(untaxed.automatic_tax->disabled_reason, "finalization_requires_location_inputs");
    EXPECT_FALSE(subscription.automatic_tax_enabled);
    EXPECT_EQ(subscription.automatic_tax_disabled_reason, "requires_location_inputs");
    const auto disabled = client.subscription_schedules().retrieve_async(schedule.id).result();
    EXPECT_EQ(disabled.status, "active");
    ASSERT_TRUE(disabled.default_automatic_tax.has_value());
    EXPECT_FALSE(disabled.default_automatic_tax->enabled);
    EXPECT_EQ(disabled.default_automatic_tax->disabled_reason, "requires_location_inputs");
    ASSERT_FALSE(disabled.phases.empty());
    ASSERT_TRUE(disabled.phases.front().automatic_tax.has_value());
    EXPECT_FALSE(disabled.phases.front().automatic_tax->enabled);
    EXPECT_EQ(disabled.phases.front().automatic_tax->disabled_reason, "requires_location_inputs");

    correct_tax_location(*environment, customer, fixtures.new_zealand);
    ASSERT_TRUE(disabled.current_phase.has_value());
    SubscriptionSchedulePhaseRequest phase;
    phase.start_date = disabled.current_phase->start_date_unix;
    phase.end_date = disabled.current_phase->end_date_unix;
    phase.items = disabled.phases.front().items;
    phase.automatic_tax_enabled = true;
    const auto restored =
        client.subscription_schedules()
            .update_async(schedule.id,
                          { .phases = { phase }, .default_automatic_tax_enabled = true })
            .result();
    ASSERT_TRUE(restored.default_automatic_tax.has_value());
    EXPECT_TRUE(restored.default_automatic_tax->enabled);
    EXPECT_TRUE(restored.default_automatic_tax->disabled_reason.empty());
    ASSERT_FALSE(restored.phases.empty());
    ASSERT_TRUE(restored.phases.front().automatic_tax.has_value());
    EXPECT_TRUE(restored.phases.front().automatic_tax->enabled);
    EXPECT_TRUE(restored.phases.front().automatic_tax->disabled_reason.empty());
    subscription = client.subscriptions().retrieve_async(subscription.id).result();
    EXPECT_TRUE(subscription.automatic_tax_enabled);
    EXPECT_TRUE(subscription.automatic_tax_disabled_reason.empty());

    const auto next_renewal = subscription.items.front().current_period_end_unix.value_or(
        subscription.current_period_end_unix);
    ASSERT_TRUE(next_renewal > renewal);
    advance_sandbox_clock(*environment, clock, next_renewal + 3600);
    subscription = client.subscriptions().retrieve_async(subscription.id).result();
    EXPECT_NE(subscription.latest_invoice_id, untaxed.id);
    expect_tax_result(*environment, subscription.latest_invoice_id, true);
    // Correcting future billing cannot retroactively fix an already-paid untaxed invoice.
    const auto unchanged = client.invoices().retrieve_async(untaxed.id).result();
    ASSERT_TRUE(unchanged.automatic_tax.has_value());
    EXPECT_FALSE(unchanged.automatic_tax->enabled);
    EXPECT_EQ(unchanged.automatic_tax->disabled_reason, "finalization_requires_location_inputs");
}
