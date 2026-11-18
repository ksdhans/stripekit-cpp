/** TestCatalog.cpp
 *
 * Sandbox tests for reusable products, prices, and their use in billing.
 */

#include "SandboxEnvironment.h"
#include "TestFramework.h"

#include "StripeKit/Errors.h"

#include <algorithm>
#include <future>
#include <chrono>
#include <thread>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace stripekit;
using namespace stripekit::testing;

class SandboxCatalogTest : public ::testing::Test {
protected:
    /** Uses the sandbox client and account selected for this run. */
    void SetUp() override {
        environment = &SandboxEnvironment::get();
    }

    /**
     * Closes test sessions and subscriptions, then archives catalog objects.
     * Cleanup failures fail the test, after attempting the remaining cleanup.
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
        for (const auto& id : sessions) {
            cleanup(id, [&] {
                environment->stripe_api().require_post("/v1/checkout/sessions/" + id + "/expire",
                                                       {});
            });
        }
        for (const auto& id : products_with_default) {
            cleanup(id, [&] {
                environment->stripe_api().require_post("/v1/products/" + id,
                                                       { { "default_price", "" } });
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
        for (const auto& id : clocks) {
            cleanup(id, [&] {
                environment->stripe_api().require_delete("/v1/test_helpers/test_clocks/" + id);
            });
        }
        subscriptions.clear();
        sessions.clear();
        prices.clear();
        products.clear();
        products_with_default.clear();
        clocks.clear();
        if (!failures.empty()) {
            throw std::runtime_error("Sandbox catalog cleanup failed:" + failures);
        }
    }

    /** Creates a run-specific product and records it for archiving. */
    Product create_product(const std::string& label) {
        CreateProductRequest request;
        request.id = "stripekit_" + environment->run_id() + "_" + label;
        request.name = "StripeKit E2E " + label;
        request.description = "Sandbox catalog test";
        request.metadata = { { "stripekit_run_id", environment->run_id() },
                             { "remove_me", "yes" } };
        RequestOptions options;
        options.idempotency_key = request.id;
        const auto product =
            environment->stripe_client().products().create_async(request, options).result();
        products.push_back(product.id);
        return product;
    }

    /**
     * Creates a fixed USD price and records it for archiving.
     * An empty interval creates a one-time price.
     */
    Price create_price(const Product& product,
                       const std::string& label,
                       std::int64_t amount = 1500,
                       const std::string& interval = "month",
                       std::int64_t interval_count = 1) {
        const auto request = price_request(product, label, amount, interval, interval_count);
        RequestOptions options;
        options.idempotency_key = request.lookup_key;
        const auto price =
            environment->stripe_client().prices().create_async(request, options).result();
        prices.push_back(price.id);
        return price;
    }

    /** Returns price inputs with a run-specific lookup key and test metadata. */
    CreatePriceRequest price_request(const Product& product,
                                     const std::string& label,
                                     std::int64_t amount,
                                     const std::string& interval,
                                     std::int64_t interval_count = 1) const {
        CreatePriceRequest request;
        request.product_id = product.id;
        request.currency = "usd";
        request.unit_amount = amount;
        request.recurring_interval = interval;
        request.recurring_interval_count = interval_count;
        request.lookup_key = environment->run_id() + "_" + label;
        request.tax_behavior = "exclusive";
        request.metadata = { { "stripekit_run_id", environment->run_id() } };
        return request;
    }

    /** Creates a paid subscription and records it before checking the returned state. */
    Subscription create_subscription(const Price& price,
                                     const std::string& label,
                                     std::int64_t quantity = 3,
                                     const std::string& clock_id = "") {
        FormParams customer_params{ { "email", sandbox_email(*environment, label) },
                                    { "payment_method", "pm_card_visa" },
                                    { "invoice_settings[default_payment_method]", "pm_card_visa" },
                                    { "metadata[stripekit_run_id]", environment->run_id() } };
        if (!clock_id.empty()) customer_params.emplace_back("test_clock", clock_id);
        const auto customer =
            environment->stripe_api().require_post("/v1/customers", customer_params);
        const auto created = environment->stripe_api().require_post(
            "/v1/subscriptions",
            { { "customer", customer.id() },
              { "items[0][price]", price.id },
              { "items[0][quantity]", std::to_string(quantity) },
              { "metadata[stripekit_run_id]", environment->run_id() } });
        subscriptions.push_back(created.id());
        const auto subscription =
            environment->stripe_client().subscriptions().retrieve_async(created.id()).result();
        EXPECT_EQ(subscription.status, "active");
        EXPECT_EQ(subscription.items.size(), 1U);
        EXPECT_EQ(subscription.items.front().quantity.value(), quantity);
        EXPECT_FALSE(subscription.latest_invoice_id.empty());
        return subscription;
    }

    /** Attaches a fresh test payment method and selects it for subsequent invoices. */
    void set_payment_method(const Subscription& subscription, const std::string& test_method) {
        const auto attached =
            environment->stripe_api().require_post("/v1/payment_methods/" + test_method + "/attach",
                                                   { { "customer", subscription.customer_id } });
        environment->stripe_api().require_post("/v1/subscriptions/" + subscription.id,
                                               { { "default_payment_method", attached.id() } });
    }

    /** Builds a single-item change that invoices immediately unless another policy is supplied. */
    UpdateSubscriptionRequest
    migration_request(const Subscription& subscription,
                      const Price& price,
                      PaymentBehavior payment,
                      ProrationBehavior proration = ProrationBehavior::AlwaysInvoice) {
        return { .items = { { .item_id = subscription.items.front().id,
                              .price_id = price.id,
                              .quantity = subscription.items.front().quantity.value() } },
                 .proration_behavior = proration,
                 .payment_behavior = payment };
    }

    /** Reads a raw object, failing explicitly on API errors rather than parsing error payloads. */
    Json::Value get_object(const std::string& path) {
        const auto result = environment->stripe_api().get(path);
        if (!result.ok())
            throw std::runtime_error("Stripe rejected GET " + path + " - " + result.describe());
        return result.body;
    }

    /** Checks persisted item identity, quantity, and price after an update. */
    Subscription check_price(const Subscription& original, const Price& price) {
        const auto saved =
            environment->stripe_client().subscriptions().retrieve_async(original.id).result();
        EXPECT_EQ(saved.items.size(), 1U);
        EXPECT_EQ(saved.items.front().id, original.items.front().id);
        EXPECT_EQ(saved.items.front().quantity, original.items.front().quantity);
        EXPECT_EQ(saved.items.front().price_id, price.id);
        return saved;
    }

    /** Waits for an invoice-specific verified event, excluding the initial subscription invoice. */
    void check_invoice_event(const std::string& type, const std::string& invoice_id) {
        const auto event = environment->wait_for_event(type, invoice_id);
        EXPECT_TRUE(event.has_value());
        EXPECT_EQ(json_path(event->json(), "data.object.id").asString(), invoice_id);
    }

    /** Checks a price-specific subscription event, avoiding earlier payment-method updates. */
    void check_subscription_event(const Subscription& subscription, const Price& replacement) {
        const auto event =
            environment->wait_for_event("customer.subscription.updated", replacement.id);
        EXPECT_TRUE(event.has_value());
        EXPECT_EQ(json_path(event->json(), "data.object.id").asString(), subscription.id);
    }

    /** Freezes billing on 1 January 2026 so prorations have deterministic amounts. */
    std::string create_clock() {
        const auto clock = create_sandbox_clock(*environment, 1767225600);
        clocks.push_back(clock);
        return clock;
    }

    /** Advances to 16 January and waits for Stripe to finish processing the test clock. */
    void advance_clock(const std::string& clock) {
        advance_sandbox_clock(*environment, clock, 1768521600);
    }

    SandboxEnvironment* environment = nullptr;
    std::vector<std::string> products;
    std::vector<std::string> products_with_default;
    std::vector<std::string> prices;
    std::vector<std::string> sessions;
    std::vector<std::string> subscriptions;
    std::vector<std::string> clocks;
};

TEST_F(SandboxCatalogTest, ProductCreationRetryRetrievalAndUpdates) {
    const auto product = create_product("product_lifecycle");
    EXPECT_FALSE(product.id.empty());
    EXPECT_EQ(product.name, "StripeKit E2E product_lifecycle");
    EXPECT_EQ(product.description, "Sandbox catalog test");
    EXPECT_TRUE(product.active);
    EXPECT_FALSE(product.deleted);
    EXPECT_FALSE(product.livemode);
    EXPECT_EQ(product.metadata.at("stripekit_run_id"), environment->run_id());

    // The helper repeats the unchanged request with the same idempotency key.
    const auto retried = create_product("product_lifecycle");
    products.pop_back();
    EXPECT_EQ(retried.id, product.id);

    std::promise<CallbackResult<Product>> completion;
    auto result = completion.get_future();
    environment->stripe_client().products().retrieve(
        product.id, {}, [&completion](CallbackResult<Product> value) {
            completion.set_value(std::move(value));
        });
    const auto retrieved_result = result.get();
    if (std::holds_alternative<std::exception_ptr>(retrieved_result)) {
        std::rethrow_exception(std::get<std::exception_ptr>(retrieved_result));
    }
    EXPECT_EQ(std::get<Product>(retrieved_result).id, product.id);
    EXPECT_EQ(std::get<Product>(retrieved_result).metadata, product.metadata);

    UpdateProductRequest rename;
    rename.name = "Renamed & expanded + plan";
    const auto renamed =
        environment->stripe_client().products().update_async(product.id, rename).result();
    EXPECT_EQ(renamed.name, *rename.name);
    EXPECT_EQ(renamed.description, product.description);
    EXPECT_TRUE(renamed.active);
    EXPECT_EQ(renamed.metadata, product.metadata);

    UpdateProductRequest changes;
    changes.description = "";
    changes.active = false;
    changes.metadata = { { "remove_me", "" }, { "updated", "yes & +" } };
    environment->stripe_client().products().update_async(product.id, changes).result();
    const auto updated =
        environment->stripe_client().products().retrieve_async(product.id).result();
    EXPECT_EQ(updated.name, *rename.name);
    EXPECT_TRUE(updated.description.empty());
    EXPECT_FALSE(updated.active);
    EXPECT_EQ(updated.metadata.count("remove_me"), 0U);
    EXPECT_EQ(updated.metadata.at("updated"), "yes & +");
    EXPECT_EQ(updated.metadata.at("stripekit_run_id"), environment->run_id());
}

TEST_F(SandboxCatalogTest, FixedPricesRoundTripAndCreationIsIdempotent) {
    const auto product = create_product("price_round_trip");
    const auto recurring = create_price(product, "quarterly", 4500, "month", 3);
    const auto request = price_request(product, "quarterly", 4500, "month", 3);
    RequestOptions options;
    options.idempotency_key = request.lookup_key;
    const auto retried =
        environment->stripe_client().prices().create_async(request, options).result();
    EXPECT_EQ(retried.id, recurring.id);

    const auto fetched =
        environment->stripe_client().prices().retrieve_async(recurring.id).result();
    EXPECT_EQ(fetched.product_id, product.id);
    EXPECT_EQ(fetched.currency, "usd");
    EXPECT_TRUE(fetched.unit_amount.has_value());
    EXPECT_EQ(*fetched.unit_amount, 4500);
    EXPECT_EQ(fetched.type, "recurring");
    EXPECT_EQ(fetched.billing_scheme, "per_unit");
    EXPECT_EQ(fetched.tax_behavior, "exclusive");
    EXPECT_EQ(fetched.lookup_key, request.lookup_key);
    EXPECT_TRUE(fetched.active);
    EXPECT_FALSE(fetched.livemode);
    EXPECT_TRUE(fetched.recurring.has_value());
    EXPECT_EQ(fetched.recurring->interval, "month");
    EXPECT_EQ(fetched.recurring->interval_count, 3);
    EXPECT_EQ(fetched.recurring->usage_type, "licensed");
    EXPECT_FALSE(fetched.transform_quantity.has_value());
    EXPECT_FALSE(fetched.custom_unit_amount);
    EXPECT_EQ(fetched.metadata, request.metadata);

    const auto one_time = create_price(product, "one_time", 2500, "");
    const auto fetched_once =
        environment->stripe_client().prices().retrieve_async(one_time.id).result();
    EXPECT_EQ(fetched_once.product_id, product.id);
    EXPECT_EQ(fetched_once.currency, "usd");
    EXPECT_TRUE(fetched_once.unit_amount.has_value());
    EXPECT_EQ(*fetched_once.unit_amount, 2500);
    EXPECT_EQ(fetched_once.type, "one_time");
    EXPECT_FALSE(fetched_once.recurring.has_value());
    EXPECT_FALSE(fetched_once.livemode);
}

TEST_F(SandboxCatalogTest, LookupFiltersPaginationAndArchivedPrices) {
    const auto product = create_product("price_listing");
    const auto first = create_price(product, "lookup & + /?=%", 1500);
    const auto second = create_price(product, "lookup_second", 1600);
    const auto archived = create_price(product, "lookup_archived", 1700);
    environment->stripe_api().require_post("/v1/prices/" + archived.id, { { "active", "false" } });

    ListPricesRequest query;
    query.product_id = product.id;
    query.lookup_keys = { first.lookup_key, second.lookup_key };
    const auto matched = environment->stripe_client().prices().list_async(query).result();
    EXPECT_EQ(matched.data.size(), 2U);
    EXPECT_FALSE(matched.has_more);
    std::set<std::string> matched_ids;
    for (const auto& price : matched.data) {
        EXPECT_EQ(price.product_id, product.id);
        matched_ids.insert(price.id);
    }
    EXPECT_EQ(matched_ids, (std::set<std::string>{ first.id, second.id }));

    query.lookup_keys.clear();
    query.limit = 1;
    std::set<std::string> paged_ids;
    for (int page_number = 0; page_number < 3; ++page_number) {
        const auto page = environment->stripe_client().prices().list_async(query).result();
        EXPECT_EQ(page.data.size(), 1U);
        EXPECT_EQ(page.data.front().product_id, product.id);
        EXPECT_TRUE(paged_ids.insert(page.data.front().id).second);
        EXPECT_EQ(page.has_more, page_number < 2);
        query.starting_after = page.data.back().id;
    }
    EXPECT_EQ(paged_ids, (std::set<std::string>{ first.id, second.id, archived.id }));

    query.starting_after.clear();
    query.limit = 10;
    query.active = false;
    const auto inactive = environment->stripe_client().prices().list_async(query).result();
    EXPECT_EQ(inactive.data.size(), 1U);
    EXPECT_EQ(inactive.data.front().id, archived.id);
    EXPECT_FALSE(inactive.data.front().active);
    EXPECT_FALSE(inactive.has_more);
    query.active = true;
    const auto active = environment->stripe_client().prices().list_async(query).result();
    EXPECT_EQ(active.data.size(), 2U);
    for (const auto& price : active.data) {
        EXPECT_TRUE(price.active);
        EXPECT_TRUE(price.id == first.id || price.id == second.id);
    }
    EXPECT_FALSE(active.has_more);
    const auto fetched = environment->stripe_client().prices().retrieve_async(archived.id).result();
    EXPECT_EQ(fetched.id, archived.id);
    EXPECT_FALSE(fetched.active);

    query.lookup_keys = { environment->run_id() + "_does_not_exist" };
    const auto empty = environment->stripe_client().prices().list_async(query).result();
    EXPECT_TRUE(empty.data.empty());
    EXPECT_FALSE(empty.has_more);
}

TEST_F(SandboxCatalogTest, CatalogPricesAreAcceptedByHostedCheckout) {
    const auto product = create_product("checkout");
    const auto recurring = create_price(product, "checkout_monthly", 1500);
    const auto one_time = create_price(product, "checkout_once", 2500, "");
    for (const auto& price : { recurring, one_time }) {
        CreateCheckoutSessionRequest request;
        request.mode = price.recurring ? "subscription" : "payment";
        request.price_id = price.id;
        request.quantity = 2;
        request.success_url = "https://example.com/success";
        request.cancel_url = "https://example.com/cancel";
        request.metadata = { { "stripekit_run_id", environment->run_id() } };
        const auto session =
            environment->stripe_client().checkout().sessions().create_async(request).result();
        sessions.push_back(session.id);
        EXPECT_FALSE(session.id.empty());
        EXPECT_FALSE(session.url.empty());
        EXPECT_EQ(session.mode, request.mode);
        EXPECT_EQ(session.status, "open");
        EXPECT_EQ(session.currency, price.currency);
        EXPECT_EQ(session.amount_total, *price.unit_amount * request.quantity);
        EXPECT_FALSE(session.livemode);

        const auto items =
            environment->stripe_api().get("/v1/checkout/sessions/" + session.id + "/line_items");
        EXPECT_TRUE(items.ok());
        EXPECT_EQ(items.body["data"].size(), 1U);
        EXPECT_EQ(items.body["data"][0]["price"]["id"].asString(), price.id);
        EXPECT_EQ(items.body["data"][0]["quantity"].asInt64(), request.quantity);
    }
}

TEST_F(SandboxCatalogTest, NewPriceDoesNotRepriceExistingSubscription) {
    const auto product = create_product("grandfathering");
    const auto original = create_price(product, "original", 1500);
    const auto customer_id = create_customer_with_card(*environment, "pm_card_visa", "catalog");
    const auto created = environment->stripe_api().require_post(
        "/v1/subscriptions",
        { { "customer", customer_id },
          { "items[0][price]", original.id },
          { "payment_behavior", "allow_incomplete" },
          { "metadata[stripekit_run_id]", environment->run_id() } });
    subscriptions.push_back(created.id());
    EXPECT_FALSE(created.id().empty());
    EXPECT_EQ(created.body["status"].asString(), "active");
    EXPECT_EQ(created.body["items"]["data"][0]["price"]["id"].asString(), original.id);
    const auto paid = environment->wait_for_event("invoice.paid", customer_id);
    EXPECT_TRUE(paid.has_value());
    EXPECT_EQ(json_path(paid->json(), "data.object.amount_paid").asInt64(), 1500);

    const auto replacement = create_price(product, "replacement", 2000);
    EXPECT_FALSE(replacement.id == original.id);
    const auto existing = environment->stripe_api().get("/v1/subscriptions/" + created.id());
    EXPECT_TRUE(existing.ok());
    EXPECT_EQ(existing.body["status"].asString(), "active");
    EXPECT_EQ(existing.body["items"]["data"].size(), 1U);
    const auto& retained = existing.body["items"]["data"][0]["price"];
    EXPECT_EQ(retained["id"].asString(), original.id);
    EXPECT_EQ(retained["unit_amount"].asInt64(), 1500);
    EXPECT_EQ(retained["recurring"]["interval"].asString(), "month");
    EXPECT_EQ(retained["product"].asString(), product.id);
    const auto saved = environment->stripe_client().prices().retrieve_async(original.id).result();
    EXPECT_TRUE(saved.active);
    EXPECT_TRUE(saved.unit_amount.has_value());
    EXPECT_EQ(*saved.unit_amount, 1500);
}

TEST_F(SandboxCatalogTest, MissingCatalogObjectsReportStripeErrors) {
    const auto missing_id = "stripekit_missing_" + environment->run_id();
    for (const bool product : { true, false }) {
        bool rejected = false;
        try {
            if (product) {
                environment->stripe_client().products().retrieve_async(missing_id).result();
            } else {
                environment->stripe_client().prices().retrieve_async(missing_id).result();
            }
        } catch (const StripeApiException& error) {
            rejected = true;
            EXPECT_EQ(error.payload().http_status, 404);
            EXPECT_EQ(error.payload().type, StripeErrorType::InvalidRequest);
            EXPECT_EQ(error.payload().code, "resource_missing");
            EXPECT_FALSE(error.payload().request_id.empty());
            EXPECT_FALSE(error.retryable());
        }
        EXPECT_TRUE(rejected);
    }
}

TEST_F(SandboxCatalogTest, DefaultPriceAndMutablePriceDetailsRoundTrip) {
    const auto product = create_product("default_price");
    const auto original = create_price(product, "default_original", 1500);
    const auto replacement = create_price(product, "default_replacement", 2000);
    const auto updated = environment->stripe_client()
                             .products()
                             .update_async(product.id, { .default_price_id = replacement.id })
                             .result();
    products_with_default.push_back(product.id);
    EXPECT_EQ(updated.default_price_id, replacement.id);
    EXPECT_EQ(environment->stripe_client()
                  .products()
                  .retrieve_async(product.id)
                  .result()
                  .default_price_id,
              replacement.id);
    UpdatePriceRequest changes;
    changes.nickname = "Replacement & monthly";
    changes.lookup_key = original.lookup_key;
    changes.transfer_lookup_key = true;
    changes.metadata = { { "updated", "yes" } };
    const auto moved =
        environment->stripe_client().prices().update_async(replacement.id, changes).result();
    EXPECT_EQ(moved.lookup_key, original.lookup_key);
    EXPECT_EQ(moved.nickname, *changes.nickname);
    EXPECT_EQ(moved.metadata.at("updated"), "yes");
    EXPECT_TRUE(environment->stripe_client()
                    .prices()
                    .retrieve_async(original.id)
                    .result()
                    .lookup_key.empty());
    const auto saved =
        environment->stripe_client().prices().retrieve_async(replacement.id).result();
    EXPECT_EQ(saved.nickname, *changes.nickname);
    EXPECT_EQ(saved.lookup_key, original.lookup_key);
    EXPECT_EQ(saved.metadata.at("updated"), "yes");
    UpdatePriceRequest clear;
    clear.nickname = "";
    clear.lookup_key = "";
    clear.metadata["updated"] = "";
    environment->stripe_client().prices().update_async(replacement.id, clear).result();
    const auto cleared =
        environment->stripe_client().prices().retrieve_async(replacement.id).result();
    EXPECT_TRUE(cleared.nickname.empty());
    EXPECT_TRUE(cleared.lookup_key.empty());
    EXPECT_EQ(cleared.metadata.count("updated"), 0U);
    EXPECT_EQ(cleared.metadata.at("stripekit_run_id"), environment->run_id());
    environment->stripe_client().prices().update_async(original.id, { .active = false }).result();
    EXPECT_FALSE(environment->stripe_client().prices().retrieve_async(original.id).result().active);
    environment->stripe_client().prices().update_async(original.id, { .active = true }).result();
    EXPECT_TRUE(environment->stripe_client().prices().retrieve_async(original.id).result().active);
    EXPECT_EQ(environment->stripe_client()
                  .prices()
                  .retrieve_async(replacement.id)
                  .result()
                  .unit_amount.value(),
              2000);
}

TEST_F(SandboxCatalogTest, ExplicitSubscriptionMigrationPreservesQuantityAndOtherItems) {
    const auto product = create_product("migration");
    const auto original = create_price(product, "migration_original", 1500);
    const auto replacement = create_price(product, "migration_replacement", 2000);
    const auto extra = create_price(product, "migration_extra", 500);
    const auto customer = create_customer_with_card(*environment, "pm_card_visa", "migration");
    const auto created = environment->stripe_api().require_post(
        "/v1/subscriptions",
        { { "customer", customer },
          { "items[0][price]", original.id },
          { "items[0][quantity]", "3" },
          { "items[1][price]", extra.id },
          { "items[1][quantity]", "2" },
          { "metadata[stripekit_run_id]", environment->run_id() } });
    subscriptions.push_back(created.id());
    const auto before =
        environment->stripe_client().subscriptions().retrieve_async(created.id()).result();
    EXPECT_EQ(before.items.size(), 2U);
    EXPECT_FALSE(before.items_has_more);
    UpdateSubscriptionRequest request;
    for (const auto& item : before.items) {
        if (item.price_id == original.id)
            request.items.push_back({ .item_id = item.id,
                                      .price_id = replacement.id,
                                      .quantity = item.quantity.value() });
    }
    EXPECT_EQ(request.items.size(), 1U);
    request.proration_behavior = ProrationBehavior::None;
    request.payment_behavior = PaymentBehavior::AllowIncomplete;
    const RequestOptions options{ .idempotency_key = environment->run_id() + "_migration" };
    const auto changed = environment->stripe_client()
                             .subscriptions()
                             .update_async(created.id(), request, options)
                             .result();
    EXPECT_EQ(changed.status, "active");
    EXPECT_FALSE(changed.pending_update.has_value());
    EXPECT_EQ(changed.items.size(), 2U);
    for (const auto& item : changed.items) {
        EXPECT_TRUE(item.price_id == replacement.id || item.price_id == extra.id);
        EXPECT_EQ(item.quantity.value(), item.price_id == replacement.id ? 3 : 2);
    }
    const auto retried = environment->stripe_client()
                             .subscriptions()
                             .update_async(created.id(), request, options)
                             .result();
    EXPECT_EQ(retried.items.size(), changed.items.size());
    EXPECT_EQ(retried.current_period_end_unix, before.current_period_end_unix);
    EXPECT_EQ(retried.latest_invoice_id, changed.latest_invoice_id);
    const auto persisted =
        environment->stripe_client().subscriptions().retrieve_async(created.id()).result();
    EXPECT_EQ(persisted.items.size(), 2U);
    for (const auto& item : persisted.items) {
        EXPECT_TRUE(item.price_id == replacement.id || item.price_id == extra.id);
        EXPECT_EQ(item.quantity.value(), item.price_id == replacement.id ? 3 : 2);
    }
    environment->stripe_client().prices().update_async(original.id, { .active = false }).result();
    EXPECT_EQ(
        environment->stripe_client().subscriptions().retrieve_async(created.id()).result().status,
        "active");
}

// Monthly and quarterly items have different period ends. Stripe must choose the
// requested earliest or latest boundary, while advance billing can move the
// billed-through cancellation date beyond both current periods.
TEST_F(SandboxCatalogTest, CancellationBoundariesResolveMixedPeriodsAndAdvanceBilling) {
    // Give the subscription two different billing periods so the boundary choice matters.
    const auto product = create_product("cancellation_boundaries");
    const auto monthly = create_price(product, "cancellation_monthly", 1000);
    const auto quarterly = create_price(product, "cancellation_quarterly", 3000, "month", 3);
    const auto clock = create_clock();
    const auto customer =
        environment->stripe_api()
            .require_post("/v1/customers",
                          { { "test_clock", clock },
                            { "payment_method", "pm_card_visa" },
                            { "invoice_settings[default_payment_method]", "pm_card_visa" } })
            .id();
    const std::int64_t prepaid_until = 1782864000; // 1 July 2026.
    // Use a fresh subscription for each option: an earlier cancellation can shorten item periods.
    for (const auto boundary : { SubscriptionCancelAt::MinPeriodEnd,
                                 SubscriptionCancelAt::MaxPeriodEnd,
                                 SubscriptionCancelAt::MaxBilledUntil }) {
        FormParams fields{ { "customer", customer },
                           { "items[0][price]", monthly.id },
                           { "items[1][price]", quarterly.id },
                           { "billing_mode[type]", "flexible" },
                           { "metadata[stripekit_run_id]", environment->run_id() } };
        if (boundary == SubscriptionCancelAt::MaxBilledUntil) {
            // Bill six months upfront without changing the monthly and quarterly service periods.
            fields.emplace_back("billing_schedules[0][bill_until][type]", "timestamp");
            fields.emplace_back("billing_schedules[0][bill_until][timestamp]",
                                std::to_string(prepaid_until));
            fields.emplace_back("proration_behavior", "always_invoice");
        }
        const auto created = environment->stripe_api().require_post("/v1/subscriptions", fields);
        subscriptions.push_back(created.id());
        auto& api = environment->stripe_client().subscriptions();
        const auto before = api.retrieve_async(created.id()).result();
        EXPECT_EQ(before.status, "active");
        ASSERT_EQ(before.items.size(), 2U);
        // Establish the real period boundaries before asking Stripe to choose a cancellation date.
        const auto [minimum, maximum] =
            std::minmax(before.items[0].current_period_end_unix.value(),
                        before.items[1].current_period_end_unix.value());
        ASSERT_TRUE(minimum < maximum);
        const auto expected = boundary == SubscriptionCancelAt::MinPeriodEnd   ? minimum
                              : boundary == SubscriptionCancelAt::MaxPeriodEnd ? maximum
                                                                               : prepaid_until;
        if (boundary == SubscriptionCancelAt::MaxBilledUntil) {
            EXPECT_TRUE(expected > maximum);
            const auto invoice = environment->stripe_client()
                                     .invoices()
                                     .retrieve_async(before.latest_invoice_id)
                                     .result();
            EXPECT_TRUE(invoice.paid);
            EXPECT_EQ(invoice.amount_paid, 12000);
        }
        UpdateSubscriptionRequest request;
        request.cancel_at = boundary;
        request.proration_behavior = ProrationBehavior::None;
        const auto changed = api.update_async(before.id, request).result();
        EXPECT_EQ(changed.cancel_at_unix.value(), expected);
        EXPECT_EQ(changed.status, "active");
        EXPECT_FALSE(changed.livemode);
        EXPECT_EQ(api.retrieve_async(before.id).result().cancel_at_unix.value(), expected);
        // Withdrawing the cancellation must also persist, allowing normal billing to continue.
        request.cancel_at = SubscriptionCancelAt::Clear;
        EXPECT_FALSE(api.update_async(before.id, request).result().cancel_at_unix.has_value());
        EXPECT_FALSE(api.retrieve_async(before.id).result().cancel_at_unix.has_value());
    }
}

TEST_F(SandboxCatalogTest, InvalidDefaultPricesLeavePreviousDefaultUnchanged) {
    const auto product = create_product("default_validation");
    const auto original = create_price(product, "valid_default");
    const auto inactive = create_price(product, "inactive_default");
    const auto other_product = create_product("other_default");
    const auto unrelated = create_price(other_product, "unrelated_default");
    environment->stripe_client()
        .products()
        .update_async(product.id, { .default_price_id = original.id })
        .result();
    products_with_default.push_back(product.id);
    environment->stripe_client().prices().update_async(inactive.id, { .active = false }).result();
    for (const auto& price : { inactive, unrelated }) {
        try {
            environment->stripe_client()
                .products()
                .update_async(product.id, { .default_price_id = price.id })
                .result();
            FAIL() << "Stripe accepted an invalid default price";
        } catch (const StripeApiException& error) {
            EXPECT_EQ(error.payload().http_status, 400);
            EXPECT_EQ(error.payload().type, StripeErrorType::InvalidRequest);
        }
        EXPECT_EQ(environment->stripe_client()
                      .products()
                      .retrieve_async(product.id)
                      .result()
                      .default_price_id,
                  original.id);
    }
}

TEST_F(SandboxCatalogTest, FailedPaymentKeepsMandatoryNewPriceAndCanBeRecovered) {
    const auto product = create_product("allow_failed");
    const auto original = create_price(product, "allow_monthly");
    const auto replacement = create_price(product, "allow_yearly", 18000, "year");
    const auto before = create_subscription(original, "allow_failed");
    set_payment_method(before, "pm_card_chargeCustomerFail");
    const auto request = migration_request(before, replacement, PaymentBehavior::AllowIncomplete);
    const RequestOptions options{ .idempotency_key = environment->run_id() + "_allow_failed" };
    const auto changed = environment->stripe_client()
                             .subscriptions()
                             .update_async(before.id, request, options)
                             .result();
    EXPECT_EQ(changed.status, "past_due");
    EXPECT_FALSE(changed.pending_update.has_value());
    EXPECT_FALSE(changed.latest_invoice_id == before.latest_invoice_id);
    EXPECT_EQ(check_price(before, replacement).status, "past_due");
    check_subscription_event(before, replacement);
    const auto invoice = get_object("/v1/invoices/" + changed.latest_invoice_id);
    EXPECT_EQ(invoice["status"].asString(), "open");
    // Invoice the annual term now, crediting the unused monthly term: 54000 - 4500.
    EXPECT_EQ(invoice["amount_due"].asInt64(), 49500);
    EXPECT_EQ(invoice["amount_remaining"].asInt64(), 49500);
    const auto intent = get_object("/v1/payment_intents/" +
                                   invoice_payment_intent(*environment, changed.latest_invoice_id));
    EXPECT_EQ(intent["status"].asString(), "requires_payment_method");
    check_invoice_event("invoice.payment_failed", changed.latest_invoice_id);
    const auto retried = environment->stripe_client()
                             .subscriptions()
                             .update_async(before.id, request, options)
                             .result();
    EXPECT_EQ(retried.latest_invoice_id, changed.latest_invoice_id);
    EXPECT_EQ(check_price(before, replacement).status, "past_due");
    set_payment_method(before, "pm_card_visa");
    const auto paid = environment->stripe_api().require_post(
        "/v1/invoices/" + changed.latest_invoice_id + "/pay", {});
    EXPECT_EQ(paid.body["status"].asString(), "paid");
    EXPECT_EQ(check_price(before, replacement).status, "active");
    check_invoice_event("invoice.paid", changed.latest_invoice_id);
}

TEST_F(SandboxCatalogTest, PendingPriceChangeAppliesOnlyAfterInvoicePayment) {
    const auto product = create_product("pending_failed");
    const auto original = create_price(product, "pending_monthly");
    const auto replacement = create_price(product, "pending_yearly", 18000, "year");
    const auto before = create_subscription(original, "pending_failed");
    set_payment_method(before, "pm_card_chargeCustomerFail");
    const auto request =
        migration_request(before, replacement, PaymentBehavior::PendingIfIncomplete);
    const auto changed =
        environment->stripe_client().subscriptions().update_async(before.id, request).result();
    EXPECT_EQ(changed.items.front().price_id, original.id);
    EXPECT_TRUE(changed.pending_update.has_value());
    EXPECT_TRUE(changed.pending_update->expires_at_unix > 0);
    EXPECT_EQ(changed.pending_update->items.size(), 1U);
    EXPECT_EQ(changed.pending_update->items.front().id, before.items.front().id);
    EXPECT_EQ(changed.pending_update->items.front().price_id, replacement.id);
    EXPECT_EQ(changed.pending_update->items.front().quantity, before.items.front().quantity);
    EXPECT_TRUE(check_price(before, original).pending_update.has_value());
    check_subscription_event(before, replacement);
    const auto invoice = get_object("/v1/invoices/" + changed.latest_invoice_id);
    EXPECT_EQ(invoice["status"].asString(), "open");
    EXPECT_EQ(invoice["amount_due"].asInt64(), 49500);
    check_invoice_event("invoice.payment_failed", changed.latest_invoice_id);
    set_payment_method(before, "pm_card_visa");
    const auto paid = environment->stripe_api().require_post(
        "/v1/invoices/" + changed.latest_invoice_id + "/pay", {});
    EXPECT_EQ(paid.body["status"].asString(), "paid");
    const auto saved = check_price(before, replacement);
    EXPECT_FALSE(saved.pending_update.has_value());
    EXPECT_EQ(saved.status, "active");
    check_invoice_event("invoice.paid", changed.latest_invoice_id);
}

TEST_F(SandboxCatalogTest, RejectingFailedPaymentLeavesSubscriptionUnchanged) {
    const auto product = create_product("reject_failed");
    const auto original = create_price(product, "reject_monthly");
    const auto replacement = create_price(product, "reject_yearly", 18000, "year");
    const auto before = create_subscription(original, "reject_failed");
    set_payment_method(before, "pm_card_chargeCustomerFail");
    const auto request = migration_request(before, replacement, PaymentBehavior::ErrorIfIncomplete);
    try {
        environment->stripe_client().subscriptions().update_async(before.id, request).result();
        FAIL() << "Stripe accepted a payment-dependent price change despite a decline";
    } catch (const StripeApiException& error) {
        EXPECT_EQ(error.payload().http_status, 402);
        EXPECT_EQ(error.payload().type, StripeErrorType::Card);
        EXPECT_FALSE(error.payload().request_id.empty());
    }
    const auto saved = check_price(before, original);
    EXPECT_EQ(saved.status, "active");
    EXPECT_FALSE(saved.pending_update.has_value());
    EXPECT_EQ(saved.current_period_end_unix, before.current_period_end_unix);
    EXPECT_EQ(saved.latest_invoice_id, before.latest_invoice_id);
}

TEST_F(SandboxCatalogTest, AuthenticationRequiredKeepsNewPriceAndEmitsActionRequired) {
    const auto product = create_product("authentication");
    const auto original = create_price(product, "auth_monthly");
    const auto replacement = create_price(product, "auth_yearly", 18000, "year");
    const auto before = create_subscription(original, "authentication");
    set_payment_method(before, "pm_card_authenticationRequired");
    const auto request = migration_request(before, replacement, PaymentBehavior::AllowIncomplete);
    const auto changed =
        environment->stripe_client().subscriptions().update_async(before.id, request).result();
    EXPECT_EQ(changed.status, "past_due");
    EXPECT_FALSE(changed.pending_update.has_value());
    EXPECT_EQ(check_price(before, replacement).status, "past_due");
    check_subscription_event(before, replacement);
    const auto invoice = get_object("/v1/invoices/" + changed.latest_invoice_id);
    EXPECT_EQ(invoice["status"].asString(), "open");
    const auto intent = get_object("/v1/payment_intents/" +
                                   invoice_payment_intent(*environment, changed.latest_invoice_id));
    EXPECT_EQ(intent["status"].asString(), "requires_action");
    EXPECT_FALSE(intent["next_action"].isNull());
    check_invoice_event("invoice.payment_action_required", changed.latest_invoice_id);
}

TEST_F(SandboxCatalogTest, DefaultIncompleteRequiresExplicitPaymentConfirmation) {
    const auto product = create_product("default_incomplete");
    const auto original = create_price(product, "confirm_monthly");
    const auto replacement = create_price(product, "confirm_yearly", 18000, "year");
    const auto before = create_subscription(original, "default_incomplete");
    const auto request = migration_request(before, replacement, PaymentBehavior::DefaultIncomplete);
    const auto changed =
        environment->stripe_client().subscriptions().update_async(before.id, request).result();
    EXPECT_EQ(changed.status, "past_due");
    EXPECT_FALSE(changed.pending_update.has_value());
    EXPECT_EQ(check_price(before, replacement).status, "past_due");
    check_subscription_event(before, replacement);
    const auto invoice = get_object("/v1/invoices/" + changed.latest_invoice_id);
    EXPECT_EQ(invoice["status"].asString(), "open");
    const auto intent_id = invoice_payment_intent(*environment, changed.latest_invoice_id);
    EXPECT_FALSE(intent_id.empty());
    const auto intent = get_object("/v1/payment_intents/" + intent_id);
    EXPECT_EQ(intent["status"].asString(), "requires_confirmation");
    const auto confirmed =
        environment->stripe_api().require_post("/v1/payment_intents/" + intent_id + "/confirm", {});
    EXPECT_EQ(confirmed.body["status"].asString(), "succeeded");
    check_invoice_event("invoice.paid", changed.latest_invoice_id);
    EXPECT_EQ(check_price(before, replacement).status, "active");
}

TEST_F(SandboxCatalogTest, ProrationPoliciesHaveDeterministicInvoiceOutcomes) {
    const auto product = create_product("proration");
    const auto original = create_price(product, "proration_original", 3100);
    const auto replacement = create_price(product, "proration_replacement", 6200);
    const auto clock = create_clock();
    const auto immediate = create_subscription(original, "proration_immediate", 3, clock);
    const auto deferred = create_subscription(original, "proration_deferred", 3, clock);
    const auto none = create_subscription(original, "proration_none", 3, clock);
    advance_clock(clock);
    for (const auto& [before, policy] : std::vector<std::pair<Subscription, ProrationBehavior>>{
             { immediate, ProrationBehavior::AlwaysInvoice },
             { deferred, ProrationBehavior::CreateProrations },
             { none, ProrationBehavior::None } }) {
        const auto request =
            migration_request(before, replacement, PaymentBehavior::AllowIncomplete, policy);
        const RequestOptions options{ .idempotency_key = environment->run_id() + "_" + before.id };
        const auto changed = environment->stripe_client()
                                 .subscriptions()
                                 .update_async(before.id, request, options)
                                 .result();
        EXPECT_EQ(changed.status, "active");
        EXPECT_EQ(changed.current_period_end_unix, before.current_period_end_unix);
        check_price(before, replacement);
        // 16 of January's 31 days remain: (6200 - 3100) * 3 * 16 / 31 = 4800.
        if (policy == ProrationBehavior::AlwaysInvoice) {
            EXPECT_FALSE(changed.latest_invoice_id == before.latest_invoice_id);
            const auto invoice = get_object("/v1/invoices/" + changed.latest_invoice_id);
            EXPECT_EQ(invoice["status"].asString(), "paid");
            EXPECT_EQ(invoice["amount_paid"].asInt64(), 4800);
            EXPECT_EQ(invoice["lines"]["data"].size(), 2U);
            std::int64_t adjustments = 0;
            for (const auto& line : invoice["lines"]["data"]) {
                const auto& parent = line["parent"];
                EXPECT_TRUE(parent[parent["type"].asString()]["proration"].asBool());
                adjustments += line["amount"].asInt64();
            }
            EXPECT_EQ(adjustments, 4800);
            check_invoice_event("invoice.paid", changed.latest_invoice_id);
        } else {
            EXPECT_EQ(changed.latest_invoice_id, before.latest_invoice_id);
        }
        const auto upcoming = environment->stripe_client()
                                  .invoices()
                                  .create_preview_async({ .subscription_id = before.id })
                                  .result();
        EXPECT_EQ(upcoming.subscription_id, before.id);
        EXPECT_EQ(upcoming.amount_due,
                  policy == ProrationBehavior::CreateProrations ? 23400 : 18600);
        EXPECT_FALSE(upcoming.id.empty());
        EXPECT_FALSE(upcoming.line_items.empty());
        const auto retried = environment->stripe_client()
                                 .subscriptions()
                                 .update_async(before.id, request, options)
                                 .result();
        EXPECT_EQ(retried.latest_invoice_id, changed.latest_invoice_id);
        const auto invoices = get_object("/v1/invoices?subscription=" + before.id);
        EXPECT_FALSE(invoices["has_more"].asBool());
        EXPECT_EQ(invoices["data"].size(), policy == ProrationBehavior::AlwaysInvoice ? 2U : 1U);
    }
}
