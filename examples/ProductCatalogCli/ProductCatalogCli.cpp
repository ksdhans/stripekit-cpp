/** ProductCatalogCli.cpp
 *
 * See README.md for details.
 */

#include "StripeKit/Adapters/Drogon.h"
#include "StripeKit/Client.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

/** Returns a required environment variable or reports which value is missing. */
std::string require_env(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || std::string_view(value).empty())
        throw std::runtime_error(std::string("Set ") + name);
    return value;
}

/** Assigns each mutation its own stable retry identity and request timeout. */
stripekit::RequestOptions options(const std::string& idempotency_key = "") {
    stripekit::RequestOptions request_options;
    if (!idempotency_key.empty()) request_options.idempotency_key = idempotency_key;
    request_options.timeout = std::chrono::seconds(30);
    return request_options;
}

/** Checks pricing terms before treating a lookup-key match as the intended offering. */
void check_price(const stripekit::Price& price,
                 const std::string& product_id,
                 const std::string& lookup_key,
                 std::int64_t amount) {
    if (price.id.empty() || price.product_id != product_id || price.lookup_key != lookup_key ||
        price.currency != "usd" || price.unit_amount != amount || price.type != "recurring" ||
        price.billing_scheme != "per_unit" || !price.recurring ||
        price.recurring->interval != "month" || price.recurring->interval_count != 1 ||
        price.recurring->usage_type != "licensed" || price.custom_unit_amount ||
        price.transform_quantity || price.livemode)
        throw std::runtime_error("Returned price does not match the expected test offering");
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Set STRIPE_SECRET_KEY=sk_test_... and STRIPEKIT_CATALOG_ID to a unique\n"
                     "label (1-64 letters, digits, underscores, or hyphens), then run without "
                     "arguments.\n"
                     "Creates test catalog objects, selects a new default price, and archives the "
                     "old price.\n";
        return 0;
    }
    try {
        if (argc != 1) throw std::runtime_error("Unknown argument. Use --help.");
        const auto api_key = require_env("STRIPE_SECRET_KEY");
        if (!api_key.starts_with("sk_test_"))
            throw std::runtime_error(
                "This example requires an sk_test_ key and refuses live keys.");
        const auto label = require_env("STRIPEKIT_CATALOG_ID");
        if (label.size() > 64 || !std::all_of(label.begin(), label.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '_' || c == '-';
            }))
            throw std::runtime_error(
                "STRIPEKIT_CATALOG_ID must contain 1-64 letters, digits, underscores, or hyphens.");

        const auto identity = "stripekit_catalog_" + label;
        const auto original_lookup = identity + "_monthly_v1";
        const auto replacement_lookup = identity + "_monthly_v2";
        auto stripe = stripekit::drogon::make_stripe_client({ .api_key = api_key });

        stripekit::CreateProductRequest product_request;
        product_request.id = identity;
        product_request.name = "StripeKit Standard";
        product_request.description = "Product catalog CLI example";
        product_request.metadata = { { "example", "product-catalog-cli" } };
        const auto created =
            stripe->products()
                .create_async(product_request, options(identity + "_create_product"))
                .result();
        std::cout << "Product: " << created.id << std::endl;

        stripekit::CreatePriceRequest price_request;
        price_request.product_id = created.id;
        price_request.currency = "usd";
        price_request.unit_amount = 2500;
        price_request.recurring_interval = "month";
        price_request.lookup_key = original_lookup;
        const auto original =
            stripe->prices().create_async(price_request, options(identity + "_create_v1")).result();
        check_price(original, created.id, original_lookup, 2500);
        std::cout << "Original price (USD 25.00/month): " << original.id << std::endl;

        stripekit::UpdateProductRequest details;
        details.name = "StripeKit Standard Plus";
        details.description = "Updated catalog display details";
        stripe->products()
            .update_async(created.id, details, options(identity + "_update_details"))
            .result();

        price_request.unit_amount = 3000;
        price_request.lookup_key = replacement_lookup;
        const auto replacement =
            stripe->prices().create_async(price_request, options(identity + "_create_v2")).result();
        check_price(replacement, created.id, replacement_lookup, 3000);
        const auto retrieved = stripe->prices().retrieve_async(replacement.id, options()).result();
        check_price(retrieved, created.id, replacement_lookup, 3000);
        if (!retrieved.active) throw std::runtime_error("Replacement price is not active");
        std::cout << "Replacement price (USD 30.00/month): " << retrieved.id << std::endl;

        stripekit::ListPricesRequest query;
        query.product_id = created.id;
        query.lookup_keys = { replacement_lookup };
        const auto page = stripe->prices().list_async(query, options()).result();
        if (page.has_more || page.data.size() != 1)
            throw std::runtime_error("Expected exactly one replacement price from the lookup key");
        check_price(page.data.front(), created.id, replacement_lookup, 3000);
        if (page.data.front().id != replacement.id || !page.data.front().active)
            throw std::runtime_error("Lookup did not return the active replacement price");
        std::cout << "Recovered by lookup key: " << page.data.front().id << std::endl;

        stripekit::UpdateProductRequest defaults;
        defaults.default_price_id = replacement.id;
        stripe->products()
            .update_async(created.id, defaults, options(identity + "_set_default"))
            .result();

        stripekit::UpdatePriceRequest archive;
        archive.active = false;
        const auto archived =
            stripe->prices()
                .update_async(original.id, archive, options(identity + "_archive_v1"))
                .result();
        if (archived.active) throw std::runtime_error("Original price was not archived");
        const auto product = stripe->products().retrieve_async(created.id, options()).result();
        if (product.default_price_id != replacement.id || product.name != *details.name)
            throw std::runtime_error("Product changes were not saved");
        std::cout << "Updated product: " << product.name << '\n'
                  << "Default price: " << product.default_price_id << '\n'
                  << "Archived original price: " << archived.id << '\n'
                  << "Use with CheckoutSessionCli: STRIPE_PRICE_ID=" << replacement.id << '\n'
                  << "Existing subscriptions have not been changed.\n"
                  << "Test product and replacement price remain active for use with Checkout.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
