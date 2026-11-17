/** TestTax.cpp
 *
 * See SandboxEnvironment.h for details.
 */
#include "SandboxEnvironment.h"
#include "TestFramework.h"
#include "StripeKit/Testing/TaxSandbox.h"

#include <chrono>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace stripekit;
using namespace stripekit::testing;

namespace {

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
                                             TaxTestResources& resources,
                                             const TaxTestFixtures& fixtures) {
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
                               { "tax_behavior", fixtures.tax_behavior },
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
                       { "pricing[price]", price },
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

/** Verify tax and collection settings on created and retrieved sessions without completing
 * Checkout. */
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
    const auto saved =
        environment.stripe_client().checkout().sessions().retrieve_async(session.id).result();
    for (const auto* result : { &session, &saved }) {
        ASSERT_TRUE(result->automatic_tax.has_value());
        EXPECT_TRUE(result->automatic_tax->enabled);
        EXPECT_EQ(result->mode, mode);
        EXPECT_EQ(result->status, "open");
        EXPECT_EQ(result->billing_address_collection, request.billing_address_collection);
        ASSERT_TRUE(result->tax_id_collection_enabled.has_value());
        EXPECT_TRUE(result->tax_id_collection_enabled.value());
    }
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
    for (const auto& profile : fixtures.profiles()) {
        const auto customer =
            create_tax_customer(environment, resources, profile.address, profile.label);
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

/** Check the service dates on the actual billed line, not just the subscription's dates. */
void expect_service_period(SandboxEnvironment& environment,
                           const std::string& invoice_id,
                           std::int64_t start,
                           std::int64_t end) {
    const auto lines = environment.stripe_api().get("/v1/invoices/" + invoice_id + "/lines");
    if (!lines.ok())
        throw std::runtime_error("Could not read recovered invoice lines: " + lines.describe());
    ASSERT_TRUE(lines.body["data"].isArray());
    ASSERT_EQ(lines.body["data"].size(), 1U);
    ASSERT_FALSE(lines.body["has_more"].asBool());
    const auto& line = lines.body["data"][0];
    ASSERT_EQ(line["amount"].asInt64(), 1000);
    ASSERT_EQ(line["period"]["start"].asInt64(), start);
    ASSERT_EQ(line["period"]["end"].asInt64(), end);
}

/** Recover a held renewal without retaining the pause credit or charging the service twice. */
void run_held_invoice_pause_recovery(SandboxEnvironment& environment,
                                     TaxTestResources& resources,
                                     const TaxTestFixtures& fixtures,
                                     const TaxTestPrices& prices,
                                     bool late,
                                     bool after_tax_disabled = false) {
    auto& client = environment.stripe_client();
    auto& api = environment.stripe_api();

    // Start with a paid period and a known billed-through date on flexible billing.
    const auto frozen = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const auto clock = create_sandbox_clock(environment, frozen);
    resources.clocks.push_back(clock);
    const auto customer =
        create_tax_customer(environment, resources, fixtures.new_zealand, "pause_recovery", clock);
    const auto first_invoice =
        create_tax_subscription(environment, resources, customer, prices.recurring);
    const auto id = resources.subscriptions.back();
    client.subscriptions().migrate_async(id).result();
    const RetrieveSubscriptionRequest expanded{ .expand = { "items.data.billed_until" } };
    auto saved = client.subscriptions().retrieve_async(id, expanded, {}).result();
    ASSERT_EQ(saved.items.size(), 1U);
    const auto item_id = saved.items.front().id;
    auto renewal = saved.items.front().current_period_end_unix.value();
    ASSERT_EQ(saved.items.front().billed_until_unix.value(), renewal);

    // Keep an unrelated credit on another paused subscription for the same customer.
    create_tax_subscription(environment, resources, customer, prices.recurring);
    const auto other_id = resources.subscriptions.back();
    client.subscriptions().migrate_async(other_id).result();
    client.subscriptions()
        .pause_async(other_id,
                     { .bill_for = { .unused_time_from = { "none" },
                                     .outstanding_usage_through = { "none" } },
                       .invoicing_behavior = "pending_invoice_item" })
        .result();
    const auto unrelated_id = api.require_post("/v1/invoiceitems",
                                               { { "customer", customer },
                                                 { "subscription", other_id },
                                                 { "amount", "-77" },
                                                 { "currency", "nzd" },
                                                 { "metadata[purpose]", "unrelated" } })
                                  .id();
    const auto unrelated = client.invoice_items().retrieve_async(unrelated_id).result();

    // Lose the tax location at renewal, then hold the new draft before it can be collected.
    client.customers()
        .update_async(
            customer,
            { .address =
                  BillingAddress{
                      .line1 = "", .city = "", .state = "", .postal_code = "", .country = "" },
              .validate_tax_location = TaxLocationValidation::Deferred })
        .result();

    // Let Stripe disable tax on an earlier renewal before testing the next held invoice.
    std::string untaxed_invoice_id;
    if (after_tax_disabled) {
        advance_sandbox_clock(environment, clock, renewal + 7200);
        saved = client.subscriptions().retrieve_async(id, expanded, {}).result();
        untaxed_invoice_id = saved.latest_invoice_id;
        ASSERT_NE(untaxed_invoice_id, first_invoice);
        const auto untaxed = client.invoices().retrieve_async(untaxed_invoice_id).result();
        ASSERT_TRUE(untaxed.paid);
        ASSERT_EQ(untaxed.amount_tax.value_or(0), 0);
        ASSERT_TRUE(untaxed.automatic_tax.has_value());
        ASSERT_FALSE(untaxed.automatic_tax->enabled);
        ASSERT_EQ(untaxed.automatic_tax->disabled_reason, "finalization_requires_location_inputs");
        ASSERT_FALSE(saved.automatic_tax_enabled);
        ASSERT_EQ(saved.automatic_tax_disabled_reason, "requires_location_inputs");
        renewal = saved.items.front().current_period_end_unix.value();
        ASSERT_EQ(saved.items.front().billed_until_unix.value(), renewal);
    }

    advance_sandbox_clock(environment, clock, renewal + 1);
    saved = client.subscriptions().retrieve_async(id, expanded, {}).result();
    const auto held_id = saved.latest_invoice_id;
    ASSERT_NE(held_id, first_invoice);
    const auto next_renewal = saved.items.front().current_period_end_unix.value();
    ASSERT_EQ(saved.items.front().billed_until_unix.value(), next_renewal);
    auto held = client.invoices().update_async(held_id, { .auto_advance = false }).result();
    ASSERT_EQ(held.status, "draft");
    ASSERT_FALSE(held.paid);
    ASSERT_TRUE(held.automatic_tax.has_value());
    if (after_tax_disabled) {
        ASSERT_FALSE(held.automatic_tax->enabled);
        ASSERT_FALSE(saved.automatic_tax_enabled);
    } else {
        ASSERT_TRUE(held.automatic_tax->enabled);
        ASSERT_EQ(held.automatic_tax->status, "requires_location_inputs");
    }
    std::set<std::string> held_lines;
    for (const auto& line : held.line_items)
        held_lines.insert(line.id);
    ASSERT_FALSE(held_lines.empty());

    // Record existing charges and pending items so only the pause's effects are attributed to it.
    const auto charges_before = client.charges().list_async({ .customer_id = customer }).result();
    ASSERT_FALSE(charges_before.has_more);
    const auto invoices_before = client.invoices().list_async({ .subscription_id = id }).result();
    ASSERT_FALSE(invoices_before.has_more);
    ASSERT_EQ(invoices_before.data.size(), after_tax_disabled ? 3U : 2U);
    const auto pending_before =
        client.invoice_items().list_async({ .customer_id = customer, .pending = true }).result();
    ASSERT_FALSE(pending_before.has_more);
    std::set<std::string> existing_items;
    for (const auto& item : pending_before.data)
        existing_items.insert(item.id);

    // Credit the whole held period and verify that pausing rolls billed_until back to its start.
    const PauseSubscriptionRequest pause{
        .bill_for = { .unused_time_from = { "item_current_period_start" },
                      .outstanding_usage_through = { "none" } },
        .invoicing_behavior = "pending_invoice_item"
    };
    ASSERT_EQ(client.subscriptions().pause_async(id, pause).result().status, "paused");
    saved = client.subscriptions().retrieve_async(id, expanded, {}).result();
    ASSERT_EQ(saved.items.front().billed_until_unix.value(), renewal);

    // Search every page for the new credit using ownership and service dates, not its sign.
    ListInvoiceItemsRequest filter{ .customer_id = customer, .pending = true, .limit = 1 };
    std::vector<InvoiceItem> credits;
    std::set<std::string> seen;
    bool has_more = false;
    do {
        const auto page = client.invoice_items().list_async(filter).result();
        has_more = page.has_more;
        for (const auto& item : page.data) {
            ASSERT_TRUE(seen.insert(item.id).second);
            if (existing_items.contains(item.id)) continue;
            if (!item.parent || !item.parent->subscription_details) continue;
            const auto& owner = *item.parent->subscription_details;
            if (owner.subscription_id != id || owner.subscription_item_id != item_id) continue;
            ASSERT_EQ(item.customer_id, customer);
            ASSERT_TRUE(item.invoice_id.empty());
            ASSERT_TRUE(item.proration);
            ASSERT_EQ(item.currency, "nzd");
            ASSERT_EQ(item.period.start_unix, renewal);
            ASSERT_EQ(item.period.end_unix, next_renewal);

            // Stripe may link the credit to invoiced lines or to a pending debit item.
            if (item.proration_details && item.proration_details->credited_items) {
                const auto& source = *item.proration_details->credited_items;
                if (source.type == "invoice_line_items") {
                    ASSERT_TRUE(source.invoice_line_item_details.has_value());
                    ASSERT_EQ(source.invoice_line_item_details->invoice_id, held_id);
                    for (const auto& line : source.invoice_line_item_details->invoice_line_item_ids)
                        ASSERT_TRUE(held_lines.contains(line));
                } else {
                    ASSERT_EQ(source.type, "invoice_item");
                    const auto debit =
                        client.invoice_items().retrieve_async(source.invoice_item_id).result();
                    ASSERT_EQ(debit.customer_id, customer);
                    ASSERT_TRUE(debit.invoice_id.empty() || debit.invoice_id == held_id);
                    ASSERT_TRUE(debit.parent.has_value());
                    ASSERT_TRUE(debit.parent->subscription_details.has_value());
                    ASSERT_EQ(debit.parent->subscription_details->subscription_id, id);
                    ASSERT_EQ(debit.parent->subscription_details->subscription_item_id, item_id);
                    ASSERT_EQ(debit.period.start_unix, renewal);
                    ASSERT_EQ(debit.period.end_unix, next_renewal);
                }
            }
            credits.push_back(item);
        }
        if (!page.data.empty()) filter.starting_after = page.data.back().id;
    } while (has_more);
    ASSERT_EQ(credits.size(), 1U);
    EXPECT_EQ(credits.front().amount, -1000);

    // Removing the credit must not undo the rollback or change the held invoice.
    const auto deleted = client.invoice_items().delete_async(credits.front().id).result();
    ASSERT_EQ(deleted.id, credits.front().id);
    ASSERT_TRUE(deleted.deleted);
    EXPECT_THROW(client.invoice_items().retrieve_async(deleted.id).result(), StripeApiException);
    saved = client.subscriptions().retrieve_async(id, expanded, {}).result();
    ASSERT_EQ(saved.status, "paused");
    ASSERT_EQ(saved.items.front().billed_until_unix.value(), renewal);
    ASSERT_EQ(client.invoices().retrieve_async(held_id).result().status, "draft");

    // Recover either within the held period or after it has ended, with valid tax restored.
    const auto recovered_at = late ? next_renewal + 86400 : renewal + 3600;
    advance_sandbox_clock(environment, clock, recovered_at);
    correct_tax_location(environment, customer, fixtures.new_zealand);
    client.invoices()
        .update_async(held_id, { .automatic_tax_enabled = true, .auto_advance = false })
        .result();
    client.subscriptions().update_async(id, { .automatic_tax_enabled = true }).result();

    // Quick recovery pays the held period; late recovery voids it and starts a fresh period.
    if (late) {
        held = client.invoices().finalize_async(held_id).result();
        ASSERT_EQ(held.status, "open");
        ASSERT_FALSE(held.paid);
        ASSERT_EQ(client.invoices().void_async(held_id).result().status, "void");
    } else {
        // Use only SDK operations: let Stripe finalize and collect, then confirm the paid webhook.
        const auto collecting =
            client.invoices().update_async(held_id, { .auto_advance = true }).result();
        ASSERT_TRUE(collecting.auto_advance.value());
        advance_sandbox_clock(environment, clock, recovered_at + 7200);
        ASSERT_TRUE(environment.wait_for_event("invoice.paid", held_id).has_value());
        expect_tax_result(environment, held_id, true);
    }

    // Neither path should create prorations when service resumes.
    const auto resumed =
        client.subscriptions()
            .resume_async(id,
                          { .billing_cycle_anchor =
                                ResumeSubscriptionBillingCycleAnchor{ late ? "now" : "unchanged" },
                            .payment_behavior = "resume_on_payment_success",
                            .proration_behavior = ProrationBehavior::None })
            .result();
    ASSERT_EQ(resumed.status, "active");
    ASSERT_FALSE(resumed.pending_update.has_value());

    // Check actual billed dates, tax, and charge counts to catch a duplicate recovery charge.
    saved = client.subscriptions().retrieve_async(id, expanded, {}).result();
    const auto paid_id = late ? saved.latest_invoice_id : held_id;
    if (late) ASSERT_NE(paid_id, held_id);
    expect_tax_result(environment, paid_id, true);
    const auto recovered_invoice = client.invoices().retrieve_async(paid_id).result();
    ASSERT_EQ(recovered_invoice.subtotal.value(), 1000);
    ASSERT_EQ(recovered_invoice.line_items.size(), 1U);
    ASSERT_EQ(saved.items.front().current_period_start_unix.value(), late ? recovered_at : renewal);
    const auto following_renewal = saved.items.front().current_period_end_unix.value();
    expect_service_period(environment, paid_id, late ? recovered_at : renewal, following_renewal);
    const auto invoices = client.invoices().list_async({ .subscription_id = id }).result();
    ASSERT_FALSE(invoices.has_more);
    ASSERT_EQ(invoices.data.size(), invoices_before.data.size() + (late ? 1U : 0U));
    const auto charges = client.charges().list_async({ .customer_id = customer }).result();
    ASSERT_FALSE(charges.has_more);
    ASSERT_EQ(charges.data.size(), charges_before.data.size() + 1);
    const auto still_pending = client.invoice_items().retrieve_async(unrelated_id).result();
    EXPECT_TRUE(still_pending.invoice_id.empty());
    EXPECT_EQ(still_pending.amount, unrelated.amount);
    EXPECT_EQ(still_pending.metadata, unrelated.metadata);

    // The following renewal must charge exactly once and leave the unrelated credit pending.
    advance_sandbox_clock(environment, clock, following_renewal + 7200);
    saved = client.subscriptions().retrieve_async(id, expanded, {}).result();
    ASSERT_EQ(saved.status, "active");
    ASSERT_NE(saved.latest_invoice_id, paid_id);
    expect_tax_result(environment, saved.latest_invoice_id, true);
    const auto following = client.invoices().retrieve_async(saved.latest_invoice_id).result();
    EXPECT_EQ(following.subtotal.value(), 1000);
    EXPECT_EQ(following.line_items.size(), 1U);
    EXPECT_EQ(saved.items.front().current_period_start_unix.value(), following_renewal);
    expect_service_period(environment,
                          saved.latest_invoice_id,
                          following_renewal,
                          saved.items.front().current_period_end_unix.value());
    const auto later_invoices = client.invoices().list_async({ .subscription_id = id }).result();
    EXPECT_FALSE(later_invoices.has_more);
    EXPECT_EQ(later_invoices.data.size(), invoices.data.size() + 1);
    const auto later_charges = client.charges().list_async({ .customer_id = customer }).result();
    EXPECT_FALSE(later_charges.has_more);
    EXPECT_EQ(later_charges.data.size(), charges.data.size() + 1);
    EXPECT_TRUE(client.invoice_items().retrieve_async(unrelated_id).result().invoice_id.empty());

    // Restoring future tax must not silently change the earlier paid, untaxed invoice.
    if (after_tax_disabled) {
        const auto unchanged = client.invoices().retrieve_async(untaxed_invoice_id).result();
        ASSERT_TRUE(unchanged.paid);
        ASSERT_TRUE(unchanged.automatic_tax.has_value());
        EXPECT_FALSE(unchanged.automatic_tax->enabled);
        EXPECT_EQ(unchanged.automatic_tax->disabled_reason,
                  "finalization_requires_location_inputs");
        EXPECT_EQ(unchanged.amount_tax.value_or(0), 0);
    }
}

} // namespace

class SandboxTaxTest : public ::testing::Test {
protected:
    /** Prepare the test-mode Tax settings before any Stripe objects are created. */
    void SetUp() override {
        environment = &SandboxEnvironment::get();
        auto& api = environment->stripe_api();
        ensure_tax_sandbox({ .get =
                                 [&](std::string_view path) {
                                     const auto response = api.get(path);
                                     if (!response.ok())
                                         throw std::runtime_error(
                                             "Could not read Stripe Tax configuration: " +
                                             response.describe());
                                     return response.body;
                                 },
                             .post =
                                 [&](std::string_view path, const TaxSandboxParams& params) {
                                     return api.require_post(path, params).body;
                                 } },
                           fixtures);
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
    const auto prices = create_tax_prices(*environment, *resources, fixtures);
    run_tax_profiles(*environment, *resources, fixtures, prices);
}

TEST_F(SandboxTaxTest, ScheduleUpdatesKeepTaxAndDiscountDurationThroughRenewal) {
    resources = std::make_unique<TaxTestResources>(*environment);
    const auto prices = create_tax_prices(*environment, *resources, fixtures);
    run_tax_schedule_renewal(*environment, *resources, fixtures, prices_for(prices, "nzd"));
}

TEST_F(SandboxTaxTest, HeldInvoiceQuickRecoveryDeletesOnlyPauseCreditAndKeepsRenewal) {
    resources = std::make_unique<TaxTestResources>(*environment);
    const auto prices = create_tax_prices(*environment, *resources, fixtures);
    run_held_invoice_pause_recovery(
        *environment, *resources, fixtures, prices_for(prices, "nzd"), false);
}

TEST_F(SandboxTaxTest, HeldInvoiceLateRecoveryDeletesOnlyPauseCreditAndResetsRenewal) {
    resources = std::make_unique<TaxTestResources>(*environment);
    const auto prices = create_tax_prices(*environment, *resources, fixtures);
    run_held_invoice_pause_recovery(
        *environment, *resources, fixtures, prices_for(prices, "nzd"), true);
}

TEST_F(SandboxTaxTest, HeldInvoiceQuickRecoveryAfterStripeDisablesTaxCollectsAutomatically) {
    resources = std::make_unique<TaxTestResources>(*environment);
    const auto prices = create_tax_prices(*environment, *resources, fixtures);
    run_held_invoice_pause_recovery(
        *environment, *resources, fixtures, prices_for(prices, "nzd"), false, true);
}

TEST_F(SandboxTaxTest, HeldInvoiceLateRecoveryAfterStripeDisablesTaxRestoresTaxedRenewals) {
    resources = std::make_unique<TaxTestResources>(*environment);
    const auto prices = create_tax_prices(*environment, *resources, fixtures);
    run_held_invoice_pause_recovery(
        *environment, *resources, fixtures, prices_for(prices, "nzd"), true, true);
}

TEST_F(SandboxTaxTest, InvalidLocationBlocksFinalizationUntilCustomerAndDraftAreCorrected) {
    resources = std::make_unique<TaxTestResources>(*environment);
    const auto prices = create_tax_prices(*environment, *resources, fixtures);
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

TEST_F(SandboxTaxTest, InvalidRenewalWebhookCanBeHeldAndCanceled) {
    resources = std::make_unique<TaxTestResources>(*environment);
    const auto prices = create_tax_prices(*environment, *resources, fixtures);
    const auto frozen = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const auto clock = create_sandbox_clock(*environment, frozen);
    resources->clocks.push_back(clock);
    const auto invalid_customer =
        create_tax_customer(*environment, *resources, fixtures.new_zealand, "tax_invalid", clock);
    const auto valid_customer =
        create_tax_customer(*environment, *resources, fixtures.new_zealand, "tax_valid", clock);
    const auto invalid_first = create_tax_subscription(
        *environment, *resources, invalid_customer, prices_for(prices, "nzd").recurring);
    const auto invalid_id = resources->subscriptions.back();
    const auto valid_first = create_tax_subscription(
        *environment, *resources, valid_customer, prices_for(prices, "nzd").recurring);
    const auto valid_id = resources->subscriptions.back();
    expect_tax_result(*environment, invalid_first, true);
    expect_tax_result(*environment, valid_first, true);

    auto& client = environment->stripe_client();
    const auto invalid_subscription = client.subscriptions().retrieve_async(invalid_id).result();
    const auto valid_subscription = client.subscriptions().retrieve_async(valid_id).result();
    ASSERT_EQ(invalid_subscription.items.size(), 1U);
    ASSERT_EQ(valid_subscription.items.size(), 1U);
    const auto renewal = invalid_subscription.items.front().current_period_end_unix.value_or(
        invalid_subscription.current_period_end_unix);
    ASSERT_TRUE(renewal > frozen);
    ASSERT_EQ(valid_subscription.items.front().current_period_end_unix.value_or(
                  valid_subscription.current_period_end_unix),
              renewal);

    // Keep a valid control while forcing the other customer into a confirmed invalid state.
    client.customers()
        .update_async(valid_customer,
                      { .validate_tax_location = TaxLocationValidation::Immediately })
        .result();
    client.customers()
        .update_async(
            invalid_customer,
            { .address =
                  BillingAddress{
                      .line1 = "", .city = "", .state = "", .postal_code = "", .country = "" },
              .validate_tax_location = TaxLocationValidation::Deferred })
        .result();
    expect_tax_location_error([&] {
        client.customers()
            .update_async(invalid_customer,
                          { .validate_tax_location = TaxLocationValidation::Immediately })
            .result();
    });

    advance_sandbox_clock(*environment, clock, renewal + 1);
    const auto invalid_invoice =
        client.subscriptions().retrieve_async(invalid_id).result().latest_invoice_id;
    const auto valid_invoice =
        client.subscriptions().retrieve_async(valid_id).result().latest_invoice_id;
    ASSERT_FALSE(invalid_invoice.empty());
    ASSERT_FALSE(valid_invoice.empty());
    ASSERT_NE(invalid_invoice, invalid_first);
    ASSERT_NE(valid_invoice, valid_first);
    const auto invalid_event = environment->wait_for_event("invoice.created", invalid_invoice);
    const auto valid_event = environment->wait_for_event("invoice.created", valid_invoice);
    ASSERT_TRUE(invalid_event.has_value());
    ASSERT_TRUE(valid_event.has_value());
    const auto invalid_envelope = invalid_event->json();
    const auto valid_envelope = valid_event->json();
    const auto& invalid_object = invalid_envelope["data"]["object"];
    const auto& valid_object = valid_envelope["data"]["object"];
    ASSERT_EQ(invalid_object["id"].asString(), invalid_invoice);
    ASSERT_EQ(valid_object["id"].asString(), valid_invoice);
    ASSERT_EQ(invalid_object["status"].asString(), "draft");
    ASSERT_EQ(valid_object["status"].asString(), "draft");
    ASSERT_TRUE(invalid_object["automatic_tax"]["enabled"].isBool());
    ASSERT_TRUE(valid_object["automatic_tax"]["enabled"].isBool());
    ASSERT_TRUE(invalid_object["automatic_tax"]["enabled"].asBool());
    ASSERT_TRUE(valid_object["automatic_tax"]["enabled"].asBool());
    // The draft reports the location problem before Stripe disables automatic tax.
    const auto& invalid_tax = invalid_object["automatic_tax"];
    const auto& valid_tax = valid_object["automatic_tax"];
    ASSERT_TRUE(invalid_tax["status"].isString());
    ASSERT_TRUE(valid_tax["status"].isString());
    EXPECT_EQ(invalid_tax["status"].asString(), "requires_location_inputs");
    EXPECT_EQ(valid_tax["status"].asString(), "complete");
    ASSERT_TRUE(invalid_tax.isMember("disabled_reason"));
    ASSERT_TRUE(valid_tax.isMember("disabled_reason"));
    EXPECT_TRUE(invalid_tax["disabled_reason"].isNull());
    EXPECT_TRUE(valid_tax["disabled_reason"].isNull());
    ASSERT_TRUE(invalid_envelope["api_version"].isString());
    ASSERT_FALSE(invalid_envelope["api_version"].asString().empty());
    EXPECT_EQ(invalid_envelope["api_version"].asString(), valid_envelope["api_version"].asString());

    // Hold the invalid draft while the valid renewal is collected normally.
    const auto held =
        client.invoices().update_async(invalid_invoice, { .auto_advance = false }).result();
    ASSERT_EQ(held.status, "draft");
    ASSERT_TRUE(held.auto_advance.has_value());
    ASSERT_FALSE(*held.auto_advance);
    advance_sandbox_clock(*environment, clock, renewal + 7200);
    expect_tax_result(*environment, valid_invoice, true);
    const auto result = client.invoices().retrieve_async(invalid_invoice).result();
    EXPECT_EQ(result.status, "draft");
    EXPECT_FALSE(result.paid);
    EXPECT_EQ(result.amount_paid, 0);
    EXPECT_EQ(result.attempt_count, 0);
    ASSERT_TRUE(result.auto_advance.has_value());
    EXPECT_FALSE(*result.auto_advance);
    const auto subscription = client.subscriptions().retrieve_async(invalid_id).result();
    EXPECT_EQ(subscription.status, "active");
    ASSERT_EQ(subscription.items.size(), 1U);
    const auto next_renewal = subscription.items.front().current_period_end_unix.value_or(
        subscription.current_period_end_unix);
    ASSERT_TRUE(next_renewal > renewal);

    // Cancel and confirm that no later invoices are created; the held draft remains.
    const auto canceled =
        environment->stripe_api().require_delete("/v1/subscriptions/" + invalid_id);
    ASSERT_EQ(std::erase(resources->subscriptions, invalid_id), 1U);
    ASSERT_EQ(canceled.body["status"].asString(), "canceled");
    const auto before = client.invoices().list_async({ .subscription_id = invalid_id }).result();
    ASSERT_FALSE(before.has_more);
    std::set<std::string> invoice_ids;
    for (const auto& invoice : before.data)
        invoice_ids.insert(invoice.id);
    EXPECT_TRUE(invoice_ids.contains(invalid_invoice));
    advance_sandbox_clock(*environment, clock, next_renewal + 7200);
    const auto after = client.invoices().list_async({ .subscription_id = invalid_id }).result();
    ASSERT_FALSE(after.has_more);
    std::set<std::string> later_invoice_ids;
    for (const auto& invoice : after.data)
        later_invoice_ids.insert(invoice.id);
    EXPECT_EQ(later_invoice_ids, invoice_ids);
    EXPECT_EQ(client.invoices().retrieve_async(invalid_invoice).result().status, "draft");
}

TEST_F(SandboxTaxTest, InvalidRenewalLocationDisablesTaxAndScheduleRecoveryRestoresIt) {
    resources = std::make_unique<TaxTestResources>(*environment);
    const auto prices = create_tax_prices(*environment, *resources, fixtures);
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
