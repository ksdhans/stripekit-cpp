/** TestSubscriptionSchedules.cpp
 *
 * See SandboxEnvironment.h for details.
 */
#include "SandboxEnvironment.h"
#include "TestFramework.h"
#include "StripeKit/Errors.h"

#include <future>

using namespace stripekit;
using namespace stripekit::testing;

namespace {
/** Wait for a callback and report its original failure to the test runner. */
SubscriptionSchedule completed(std::future<CallbackResult<SubscriptionSchedule>>& result) {
    auto value = result.get();
    if (std::holds_alternative<std::exception_ptr>(value))
        std::rethrow_exception(std::get<std::exception_ptr>(value));
    return std::get<SubscriptionSchedule>(std::move(value));
}
} // namespace

class SandboxSubscriptionSchedulesTest : public ::testing::Test {
protected:
    /** Create an isolated paid subscription and prices for future billing periods. */
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
        auto& client = environment->stripe_client();
        product = client.products()
                      .create_async({ .name = "Schedule tests " + environment->run_id() })
                      .result()
                      .id;
        for (const auto amount : { 1500, 2500, 300 }) {
            prices.push_back(client.prices()
                                 .create_async({ .product_id = product,
                                                 .currency = "usd",
                                                 .unit_amount = amount,
                                                 .recurring_interval = "month" })
                                 .result()
                                 .id);
        }
        subscription = environment->stripe_api()
                           .require_post("/v1/subscriptions",
                                         { { "customer", customer },
                                           { "items[0][price]", prices[0] },
                                           { "items[0][quantity]", "2" },
                                           { "items[1][price]", prices[2] },
                                           { "items[1][quantity]", "3" } })
                           .id();
        const auto saved = client.subscriptions().retrieve_async(subscription).result();
        if (saved.status != "active" || saved.items.size() != 2)
            throw std::runtime_error("Schedule fixture subscription was not paid");
        paid_invoice = saved.latest_invoice_id;
        renewal =
            saved.items.front().current_period_end_unix.value_or(saved.current_period_end_unix);
        if (renewal <= frozen) throw std::runtime_error("Schedule fixture has no future renewal");
    }

    /** Delete clock-bound billing objects and retire catalog fixtures, surfacing cleanup failures.
     */
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
        cleanup(coupon, [&] { environment->stripe_api().require_delete("/v1/coupons/" + coupon); });
        cleanup(tax_rate, [&] {
            environment->stripe_api().require_post("/v1/tax_rates/" + tax_rate,
                                                   { { "active", "false" } });
        });
        for (const auto& price : prices)
            cleanup(price, [&] {
                environment->stripe_client()
                    .prices()
                    .update_async(price, { .active = false })
                    .result();
            });
        cleanup(product, [&] {
            environment->stripe_client()
                .products()
                .update_async(product, { .active = false })
                .result();
        });
        if (!failures.empty()) throw std::runtime_error("Schedule cleanup failed:" + failures);
    }

    /** Confirm price quantities without depending on Stripe's item ordering. */
    void expect_items(const Subscription& saved, const std::string& main_price) {
        ASSERT_EQ(saved.items.size(), 2U);
        bool main = false, extra = false;
        for (const auto& item : saved.items) {
            if (item.price_id == main_price) {
                main = true;
                EXPECT_EQ(item.quantity.value(), 2);
            } else if (item.price_id == prices[2]) {
                extra = true;
                EXPECT_EQ(item.quantity.value(), 3);
            } else FAIL() << "Unexpected subscription price " << item.price_id;
        }
        EXPECT_TRUE(main);
        EXPECT_TRUE(extra);
    }

    /** Create a schedule through the callback API, then retry with the same key. */
    SubscriptionSchedule create_schedule() {
        auto& api = environment->stripe_client().subscription_schedules();
        auto create_options = options;
        create_options.idempotency_key = subscription + "_create_schedule";
        const CreateSubscriptionScheduleRequest request{ subscription };
        std::promise<CallbackResult<SubscriptionSchedule>> completion;
        auto result = completion.get_future();
        api.create(request, create_options, [&](CallbackResult<SubscriptionSchedule> value) {
            completion.set_value(std::move(value));
        });
        const auto saved = completed(result);
        EXPECT_EQ(saved.customer_id, customer);
        EXPECT_EQ(saved.subscription_id, subscription);
        EXPECT_EQ(saved.status, "active");
        EXPECT_TRUE(saved.metadata.empty());
        EXPECT_EQ(api.create_async(request, create_options).result().id, saved.id);
        EXPECT_EQ(environment->stripe_client()
                      .subscriptions()
                      .retrieve_async(subscription)
                      .result()
                      .schedule_id,
                  saved.id);
        EXPECT_THROW(api.create_async({ subscription }, options).result(), StripeApiException);
        return saved;
    }

    SandboxEnvironment* environment = nullptr;
    std::string clock, customer, product, subscription, paid_invoice, coupon, tax_rate;
    std::vector<std::string> prices;
    std::int64_t frozen = 0, renewal = 0;
    RequestOptions options;
};

TEST_F(SandboxSubscriptionSchedulesTest, DefaultApiVersionSupportsExplicitPhaseDates) {
    auto& schedules = environment->stripe_client().subscription_schedules();
    const auto created = schedules.create_async({ .from_subscription = subscription }).result();
    ASSERT_TRUE(created.current_phase.has_value());
    ASSERT_FALSE(created.phases.empty());
    SubscriptionSchedulePhaseRequest phase;
    phase.start_date = created.current_phase->start_date_unix;
    phase.end_date = created.current_phase->end_date_unix;
    phase.items = created.phases.front().items;
    UpdateSubscriptionScheduleRequest request;
    request.phases.push_back(std::move(phase));
    const auto updated = schedules.update_async(created.id, request).result();
    ASSERT_EQ(updated.phases.size(), 1U);
    EXPECT_EQ(updated.phases.front().start_date_unix, created.current_phase->start_date_unix);
    EXPECT_EQ(updated.phases.front().end_date_unix, created.current_phase->end_date_unix);
    EXPECT_EQ(updated.phases.front().items.size(), 2U);
    const auto saved = schedules.retrieve_async(created.id).result();
    EXPECT_EQ(saved.id, created.id);
    EXPECT_EQ(saved.subscription_id, subscription);
}

TEST_F(SandboxSubscriptionSchedulesTest,
       FuturePriceStartsAtRenewalAndReleaseKeepsSubscriptionRunning) {
    auto& client = environment->stripe_client();
    auto& api = client.subscription_schedules();
    const auto created = create_schedule();
    ASSERT_TRUE(created.current_phase.has_value());
    UpdateSubscriptionScheduleRequest request;
    request.metadata = { { "operation", subscription }, { "target_price", prices[1] } };
    request.phases = { { .start_date = created.current_phase->start_date_unix,
                         .end_date = renewal,
                         .items = { { prices[0], 2 }, { prices[2], 3 } } },
                       { .start_date = renewal,
                         .duration = SubscriptionSchedulePhaseDuration{ "month", 1 },
                         .items = { { prices[1], 2 }, { prices[2], 3 } },
                         .metadata = { { "billing_stage", "new_price" } } } };
    auto update_options = options;
    update_options.idempotency_key = created.id + "_update";
    const auto updated = api.update_async(created.id, request, update_options).result();
    ASSERT_EQ(updated.phases.size(), 2U);
    EXPECT_EQ(updated.phases[0].end_date_unix, renewal);
    EXPECT_EQ(updated.phases[1].start_date_unix, renewal);
    EXPECT_EQ(updated.phases[1].metadata.at("billing_stage"), "new_price");
    EXPECT_EQ(updated.end_behavior, "release");
    const auto release_at = updated.phases[1].end_date_unix;
    std::promise<CallbackResult<SubscriptionSchedule>> update_completion;
    auto update_result = update_completion.get_future();
    api.update(
        created.id, request, update_options, [&](CallbackResult<SubscriptionSchedule> value) {
            update_completion.set_value(std::move(value));
        });
    EXPECT_EQ(completed(update_result).phases[1].end_date_unix, release_at);
    std::promise<CallbackResult<SubscriptionSchedule>> read_completion;
    auto read_result = read_completion.get_future();
    api.retrieve(created.id, options, [&](CallbackResult<SubscriptionSchedule> value) {
        read_completion.set_value(std::move(value));
    });
    EXPECT_EQ(completed(read_result).metadata.at("target_price"), prices[1]);
    auto saved = client.subscriptions().retrieve_async(subscription).result();
    expect_items(saved, prices[0]);
    EXPECT_EQ(saved.latest_invoice_id, paid_invoice);
    EXPECT_EQ(client.invoices().retrieve_async(paid_invoice).result().amount_paid, 3900);
    advance_sandbox_clock(*environment, clock, renewal - 1);
    expect_items(client.subscriptions().retrieve_async(subscription).result(), prices[0]);
    advance_sandbox_clock(*environment, clock, renewal + 3600);
    saved = client.subscriptions().retrieve_async(subscription).result();
    expect_items(saved, prices[1]);
    EXPECT_EQ(saved.status, "active");
    EXPECT_EQ(saved.metadata.at("billing_stage"), "new_price");
    EXPECT_NE(saved.latest_invoice_id, paid_invoice);
    auto invoice = client.invoices().retrieve_async(saved.latest_invoice_id).result();
    EXPECT_EQ(invoice.status, "paid");
    EXPECT_EQ(invoice.amount_paid, 5900);
    EXPECT_EQ(api.retrieve_async(created.id, options).result().current_phase->start_date_unix,
              renewal);
    advance_sandbox_clock(*environment, clock, release_at + 3600);
    const auto released = api.retrieve_async(created.id, options).result();
    EXPECT_EQ(released.status, "released");
    EXPECT_TRUE(released.subscription_id.empty());
    EXPECT_EQ(released.released_subscription_id, subscription);
    EXPECT_FALSE(released.current_phase.has_value());
    saved = client.subscriptions().retrieve_async(subscription).result();
    EXPECT_EQ(saved.status, "active");
    EXPECT_TRUE(saved.schedule_id.empty());
    expect_items(saved, prices[1]);
    invoice = client.invoices().retrieve_async(saved.latest_invoice_id).result();
    EXPECT_EQ(invoice.status, "paid");
    EXPECT_EQ(invoice.amount_paid, 5900);
    EXPECT_THROW(api.update_async(created.id, request, options).result(), StripeApiException);
}

TEST_F(SandboxSubscriptionSchedulesTest, ExplicitFinalDateAndCancelEndBehaviorStopRenewal) {
    auto& client = environment->stripe_client();
    const auto created = create_schedule();
    UpdateSubscriptionScheduleRequest request;
    request.end_behavior = SubscriptionScheduleEndBehavior::Cancel;
    request.phases = { { .start_date = created.current_phase->start_date_unix,
                         .end_date = renewal,
                         .items = { { prices[0], 2 }, { prices[2], 3 } } } };
    const auto updated =
        client.subscription_schedules().update_async(created.id, request, options).result();
    EXPECT_EQ(updated.end_behavior, "cancel");
    EXPECT_EQ(updated.phases[0].end_date_unix, renewal);
    auto saved = client.subscriptions().retrieve_async(subscription).result();
    expect_items(saved, prices[0]);
    EXPECT_EQ(saved.status, "active");
    advance_sandbox_clock(*environment, clock, renewal + 1);
    EXPECT_EQ(client.subscriptions().retrieve_async(subscription).result().status, "canceled");
    const auto schedule =
        client.subscription_schedules().retrieve_async(created.id, options).result();
    EXPECT_EQ(schedule.status, "completed");
    EXPECT_FALSE(schedule.current_phase.has_value());
    EXPECT_EQ(client.invoices().retrieve_async(paid_invoice).result().amount_paid, 3900);
}

TEST_F(SandboxSubscriptionSchedulesTest,
       SubscriptionReadsPreserveDiscountAndTaxSettingsWhenCreatingSchedule) {
    auto& client = environment->stripe_client();
    coupon =
        environment->stripe_api()
            .require_post("/v1/coupons", { { "percent_off", "10" }, { "duration", "forever" } })
            .id();
    tax_rate = environment->stripe_api()
                   .require_post("/v1/tax_rates",
                                 { { "display_name", "Schedule test tax" },
                                   { "percentage", "5" },
                                   { "inclusive", "false" } })
                   .id();
    auto saved = client.subscriptions().retrieve_async(subscription).result();
    const auto item_id = saved.items[0].id;
    environment->stripe_api().require_post("/v1/subscriptions/" + subscription,
                                           { { "discounts[0][coupon]", coupon },
                                             { "default_tax_rates[0]", tax_rate },
                                             { "items[0][id]", item_id },
                                             { "items[0][discounts][0][coupon]", coupon },
                                             { "items[0][tax_rates][0]", tax_rate },
                                             { "proration_behavior", "none" } });
    for (const auto& read_options : { RequestOptions{}, options }) {
        saved = client.subscriptions().retrieve_async(subscription, read_options).result();
        EXPECT_EQ(saved.discount_ids.size(), 1U);
        EXPECT_EQ(saved.default_tax_rate_ids.size(), 1U);
        EXPECT_EQ(saved.default_tax_rate_ids[0], tax_rate);
        EXPECT_FALSE(saved.automatic_tax_enabled);
        bool found = false;
        for (const auto& item : saved.items) {
            if (item.id != item_id) continue;
            found = true;
            EXPECT_EQ(item.discount_ids.size(), 1U);
            EXPECT_EQ(item.tax_rate_ids.size(), 1U);
            EXPECT_EQ(item.tax_rate_ids[0], tax_rate);
        }
        EXPECT_TRUE(found);
    }
    create_schedule();
    saved = client.subscriptions().retrieve_async(subscription).result();
    EXPECT_EQ(saved.discount_ids.size(), 1U);
    EXPECT_EQ(saved.default_tax_rate_ids[0], tax_rate);
}

TEST_F(SandboxSubscriptionSchedulesTest, SubscriptionReadsPreserveEnabledAutomaticTax) {
    auto& client = environment->stripe_client();
    UpdateCustomerRequest customer_update;
    customer_update.address = BillingAddress{ .line1 = "354 Oyster Point Blvd",
                                              .city = "South San Francisco",
                                              .state = "CA",
                                              .postal_code = "94080",
                                              .country = "US" };
    customer_update.validate_tax_location = TaxLocationValidation::Immediately;
    const auto validated_customer =
        client.customers().update_async(customer, customer_update, options).result();
    ASSERT_TRUE(validated_customer.address.has_value());
    EXPECT_EQ(validated_customer.address->country.value(), "US");
    environment->stripe_api().require_post(
        "/v1/subscriptions/" + subscription,
        { { "automatic_tax[enabled]", "true" }, { "proration_behavior", "none" } });
    for (const auto& read_options : { RequestOptions{}, options }) {
        const auto saved =
            client.subscriptions().retrieve_async(subscription, read_options).result();
        EXPECT_TRUE(saved.automatic_tax_enabled);
        EXPECT_EQ(saved.customer_id, customer);
        EXPECT_EQ(saved.latest_invoice_id, paid_invoice);
        expect_items(saved, prices[0]);
    }
    auto& schedules = client.subscription_schedules();
    const auto created =
        schedules.create_async({ .from_subscription = subscription }, options).result();
    ASSERT_TRUE(created.current_phase.has_value());
    ASSERT_TRUE(created.default_automatic_tax.has_value());
    EXPECT_TRUE(created.default_automatic_tax->enabled);
    UpdateSubscriptionScheduleRequest update;
    update.default_automatic_tax_enabled = true;
    SubscriptionSchedulePhaseRequest phase;
    phase.start_date = created.current_phase->start_date_unix;
    phase.end_date = created.current_phase->end_date_unix;
    phase.automatic_tax_enabled = true;
    phase.items = created.phases.front().items;
    update.phases.push_back(std::move(phase));
    const auto changed = schedules.update_async(created.id, update, options).result();
    ASSERT_TRUE(changed.default_automatic_tax.has_value());
    EXPECT_TRUE(changed.default_automatic_tax->enabled);
    ASSERT_TRUE(changed.phases.front().automatic_tax.has_value());
    EXPECT_TRUE(changed.phases.front().automatic_tax->enabled);
}
