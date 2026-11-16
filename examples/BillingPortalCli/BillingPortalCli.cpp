/** BillingPortalCli.cpp
 *
 * See README.md for details.
 */

#include "StripeKit/Adapters/Drogon.h"
#include "StripeKit/Client.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view stripe_api_version = stripekit::default_stripe_api_version;

constexpr std::string_view usage =
    "Usage: stripekit_billing_portal_cli [options]\n"
    "\n"
    "Creates a Billing Portal session for STRIPE_CUSTOMER_ID and prints its URL.\n"
    "Set STRIPE_SECRET_KEY, STRIPE_CUSTOMER_ID, and STRIPE_RETURN_URL first.\n"
    "\n"
    "Options:\n"
    "  --flow <task>          Open the portal at one task instead of its home page:\n"
    "                         payment_method_update, subscription_cancel,\n"
    "                         subscription_update, or subscription_update_confirm\n"
    "  --subscription <id>    The sub_... to cancel or change (all tasks except\n"
    "                         payment_method_update)\n"
    "  --price <id>           The price_... to switch to (subscription_update_confirm)\n"
    "  --configuration <id>   A bpc_... portal configuration (default: account default)\n"
    "  --locale <locale>      Portal language, such as de or pt-BR (default: automatic)\n"
    "  --help                 Show this help\n";

/** The per-run choices given on the command line. */
struct Options {
    std::string flow;
    std::string subscription_id;
    std::string price_id;
    std::string configuration_id;
    std::string locale;
    bool help = false;
};

/**
 * Reads the command-line options.
 *
 * @throws std::runtime_error If an option is unknown or is missing its value.
 */
Options parse_options(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name = argv[i];
        if (name == "--help") {
            options.help = true;
            continue;
        }
        std::string* target = nullptr;
        if (name == "--flow") target = &options.flow;
        if (name == "--subscription") target = &options.subscription_id;
        if (name == "--price") target = &options.price_id;
        if (name == "--configuration") target = &options.configuration_id;
        if (name == "--locale") target = &options.locale;
        if (target == nullptr) {
            throw std::runtime_error("Unknown option " + std::string(name) + ". Use --help.");
        }
        if (i + 1 >= argc) throw std::runtime_error(std::string(name) + " needs a value.");
        *target = argv[++i];
    }
    return options;
}

/** Returns a required environment variable or reports which value is missing. */
std::string require_env(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || std::string_view(value).empty()) {
        throw std::runtime_error(std::string("Set ") + name);
    }
    return value;
}

/** Builds request options shared by this example's Stripe API calls. */
stripekit::RequestOptions request_options() {
    stripekit::RequestOptions options;
    options.timeout = std::chrono::seconds(30);
    options.headers.emplace("Stripe-Version", std::string(stripe_api_version));
    return options;
}

/**
 * Builds the portal task selected with --flow, or returns nothing to open the
 * portal's home page.
 *
 * The confirm task also fetches the subscription to find the item being changed.
 *
 * @param client Client used to read the subscription for the confirm task.
 *
 * @param options The command-line options.
 *
 * @return The flow to send with the session request, if any.
 *
 * @throws std::runtime_error If the task is unknown or a required option is missing.
 */
std::optional<stripekit::BillingPortalFlow> portal_flow(stripekit::StripeClient& client,
                                                        const Options& options) {
    using stripekit::BillingPortalFlowType;
    if (options.flow.empty()) return std::nullopt;

    stripekit::BillingPortalFlow flow;
    if (options.flow == "payment_method_update") {
        flow.type = BillingPortalFlowType::PaymentMethodUpdate;
        return flow;
    }
    if (options.flow == "subscription_cancel") {
        flow.type = BillingPortalFlowType::SubscriptionCancel;
    } else if (options.flow == "subscription_update") {
        flow.type = BillingPortalFlowType::SubscriptionUpdate;
    } else if (options.flow == "subscription_update_confirm") {
        flow.type = BillingPortalFlowType::SubscriptionUpdateConfirm;
    } else {
        throw std::runtime_error("Unknown --flow " + options.flow + ". Use --help.");
    }
    if (options.subscription_id.empty()) {
        throw std::runtime_error("--flow " + options.flow + " needs --subscription.");
    }
    flow.subscription_id = options.subscription_id;

    if (flow.type == BillingPortalFlowType::SubscriptionUpdateConfirm) {
        if (options.price_id.empty()) {
            throw std::runtime_error("--flow " + options.flow + " needs --price.");
        }
        const auto subscription =
            client.subscriptions().retrieve_async(flow.subscription_id, request_options()).result();
        if (subscription.items.size() != 1) {
            throw std::runtime_error("This example only changes single-item subscriptions.");
        }
        flow.items.push_back(
            { .item_id = subscription.items.front().id, .price_id = options.price_id });
    }
    return flow;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const Options options = parse_options(argc, argv);
        if (options.help) {
            std::cout << usage;
            return 0;
        }

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
                .configuration_id = options.configuration_id,
                .locale = options.locale,
                .flow = portal_flow(*client, options),
            },
            request_options());

        const stripekit::BillingPortalSession session = task.result();
        std::cout << "Billing Portal Session: " << session.id << '\n';
        std::cout << "Hosted URL: " << session.url << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
