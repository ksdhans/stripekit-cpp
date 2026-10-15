/** TestCatalog.cpp
 *
 * Sandbox tests for reusable products, prices, and their use in billing.
 */

#include "SandboxEnvironment.h"
#include "TestFramework.h"

#include "StripeKit/Errors.h"

#include <future>
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
        for (const auto& id : prices) {
            cleanup(id, [&] {
                environment->stripe_api().require_post("/v1/prices/" + id,
                                                       { { "active", "false" } });
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
        sessions.clear();
        prices.clear();
        products.clear();
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

    SandboxEnvironment* environment = nullptr;
    std::vector<std::string> products;
    std::vector<std::string> prices;
    std::vector<std::string> sessions;
    std::vector<std::string> subscriptions;
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
