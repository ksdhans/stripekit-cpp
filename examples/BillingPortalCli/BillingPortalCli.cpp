/** BillingPortalCli.cpp
 *
 * See README.md for details.
 */

#include "StripeKit/Adapters/Drogon.h"
#include "StripeKit/Client.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view stripe_api_version = "2024-06-20";

/** Returns a required environment variable or reports which value is missing. */
std::string require_env(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || std::string_view(value).empty()) {
        throw std::runtime_error(std::string("Set ") + name);
    }
    return value;
}

/** Builds request options shared by this example's Stripe API call. */
stripekit::RequestOptions options() {
    stripekit::RequestOptions request_options;
    request_options.idempotency_key = "stripekit_billing_portal_cli";
    request_options.timeout = std::chrono::seconds(30);
    request_options.headers.emplace("Stripe-Version", std::string(stripe_api_version));
    return request_options;
}

} // namespace

int main() {
    try {
        const std::string api_key = require_env("STRIPE_SECRET_KEY");
        if (api_key.starts_with("sk_live_")) {
            throw std::runtime_error("This example refuses live keys. Use an sk_test_ key.");
        }
        if (!api_key.starts_with("sk_test_")) {
            throw std::runtime_error("STRIPE_SECRET_KEY must begin with sk_test_.");
        }

        auto client = stripekit::drogon::make_stripe_client({ .api_key = api_key });
        auto task = client->billing_portal().sessions().create_async(
            stripekit::CreateBillingPortalSessionRequest{
                .customer_id = require_env("STRIPE_CUSTOMER_ID"),
                .return_url = require_env("STRIPE_RETURN_URL"),
            },
            options());

        const stripekit::BillingPortalSession session = task.result();
        std::cout << "Billing Portal Session: " << session.id << '\n';
        std::cout << "Hosted URL: " << session.url << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}