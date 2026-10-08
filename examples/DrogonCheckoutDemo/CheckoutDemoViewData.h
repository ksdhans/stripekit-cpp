/** CheckoutDemoViewData.h
 *
 * View data structures for the Drogon checkout demo.
 *
 * @author Hans de Ruiter
 *
 * @License See LICENSE for details.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace stripekit::examples {

/** One verified Stripe webhook delivery displayed by the demo. */
struct EventRecord {
    std::string id;
    std::string type;
    std::string api_version;
    bool livemode = false;
};

/** Values rendered by the checkout demo home page template. */
struct CheckoutDemoViewData {
    std::uint16_t port = 4242;
    std::string notice;
    bool has_api_key = false;
    std::string price_id;
    std::string product_name;
    std::string currency;
    std::int64_t unit_amount = 0;
    bool has_webhook_secret = false;
    bool stripe_cli_installed = false;
    bool cli_started = false;
    bool cli_starting = false;
    bool can_start_checkout = false;
    std::string configured_summary;
    std::string listener_log_path;
    std::vector<EventRecord> events;
};

/** Values rendered by the checkout return page template. */
struct ReturnPageViewData {
    std::string session_id;
};

} // namespace stripekit::examples