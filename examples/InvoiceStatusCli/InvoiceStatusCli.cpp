/** InvoiceStatusCli.cpp
 *
 * See README.md for details.
 */

#include "StripeKit/Adapters/Drogon.h"
#include "StripeKit/Client.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view stripe_api_version = stripekit::default_stripe_api_version;

constexpr std::string_view usage =
    "Usage: stripekit_invoice_status_cli (--invoice <id> | --subscription <id>)\n"
    "\n"
    "Shows what an invoice still owes, when Stripe will retry the payment, and where\n"
    "the customer can pay. Set STRIPE_SECRET_KEY first.\n"
    "\n"
    "Options:\n"
    "  --invoice <id>         The in_... invoice to show\n"
    "  --subscription <id>    Show the latest invoice of this sub_... subscription\n"
    "  --help                 Show this help\n";

/** The invoice to show, as chosen on the command line. */
struct Options {
    std::string invoice_id;
    std::string subscription_id;
    bool help = false;
};

/**
 * Reads the command-line options.
 *
 * @throws std::runtime_error If an option is unknown or missing its value, or
 *         if not exactly one of --invoice and --subscription is given.
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
        if (name == "--invoice") target = &options.invoice_id;
        if (name == "--subscription") target = &options.subscription_id;
        if (target == nullptr) {
            throw std::runtime_error("Unknown option " + std::string(name) + ". Use --help.");
        }
        if (i + 1 >= argc) throw std::runtime_error(std::string(name) + " needs a value.");
        *target = argv[++i];
    }
    if (!options.help && options.invoice_id.empty() == options.subscription_id.empty()) {
        throw std::runtime_error("Give either --invoice or --subscription. Use --help.");
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
 * Formats an amount given in the currency's smallest unit (such as cents) for
 * display, for example "15.00 USD".
 *
 * Handles Stripe's zero-decimal currencies, such as JPY. A few currencies with
 * three decimals are shown with two; a real application should use a proper
 * money formatting library.
 */
std::string format_amount(std::int64_t amount, std::string currency) {
    static constexpr std::array<std::string_view, 16> zero_decimal = { "bif", "clp", "djf", "gnf",
                                                                       "jpy", "kmf", "krw", "mga",
                                                                       "pyg", "rwf", "ugx", "vnd",
                                                                       "vuv", "xaf", "xof", "xpf" };
    const bool whole_units =
        std::find(zero_decimal.begin(), zero_decimal.end(), currency) != zero_decimal.end();
    std::transform(currency.begin(), currency.end(), currency.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    std::ostringstream text;
    if (whole_units) {
        text << amount;
    } else {
        text << amount / 100 << '.' << std::setw(2) << std::setfill('0') << std::abs(amount % 100);
    }
    text << ' ' << currency;
    return text.str();
}

/** Formats a Unix timestamp as a UTC date and time, for example "2026-10-24 09:00 UTC". */
std::string format_time(std::int64_t unix_seconds) {
    const std::time_t time = static_cast<std::time_t>(unix_seconds);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream text;
    text << std::put_time(&utc, "%Y-%m-%d %H:%M UTC");
    return text.str();
}

/**
 * Picks the invoice to show: the one given with --invoice, or the latest
 * invoice of the subscription given with --subscription.
 *
 * @throws std::runtime_error If the subscription has no invoice yet.
 */
std::string invoice_id(stripekit::StripeClient& client, const Options& options) {
    if (!options.invoice_id.empty()) return options.invoice_id;
    const auto subscription =
        client.subscriptions().retrieve_async(options.subscription_id, request_options()).result();
    if (subscription.latest_invoice_id.empty()) {
        throw std::runtime_error("Subscription " + options.subscription_id +
                                 " has no invoice yet.");
    }
    return subscription.latest_invoice_id;
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
        const stripekit::Invoice invoice =
            client->invoices()
                .retrieve_async(invoice_id(*client, options), request_options())
                .result();

        std::cout << "Invoice " << invoice.id;
        if (!invoice.number.empty()) std::cout << " (" << invoice.number << ')';
        std::cout << ": " << invoice.status << '\n';
        std::cout << "Owed: " << format_amount(invoice.amount_remaining, invoice.currency) << " of "
                  << format_amount(invoice.amount_due, invoice.currency) << '\n';
        if (invoice.due_date_unix) {
            std::cout << "Due: " << format_time(*invoice.due_date_unix) << '\n';
        }
        if (invoice.next_payment_attempt_unix) {
            std::cout << "Next automatic retry: " << format_time(*invoice.next_payment_attempt_unix)
                      << '\n';
        }
        if (!invoice.paid && invoice.status == "open" && !invoice.hosted_invoice_url.empty()) {
            std::cout << "Pay now: " << invoice.hosted_invoice_url << '\n';
        }
        if (!invoice.invoice_pdf.empty()) {
            std::cout << "PDF: " << invoice.invoice_pdf << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
