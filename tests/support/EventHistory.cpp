/** EventHistory.cpp
 *
 * See header file for details.
 */
#include "StripeKit/Testing/EventHistory.h"

#include <stdexcept>

namespace stripekit::testing {
namespace {

/** Encode event types and pagination cursors as URL query values. */
std::string query_value(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_' ||
            character == '.' || character == '~') {
            result += character;
        } else {
            result += '%';
            result += hex[character >> 4];
            result += hex[character & 15];
        }
    }
    return result;
}

} // namespace

std::string event_history_diagnostics(const std::function<Json::Value(std::string_view)>& get,
                                      std::string_view type,
                                      std::string_view must_contain,
                                      std::int64_t since_unix) {
    std::string report = "Stripe event history (created >= " + std::to_string(since_unix) +
                         ", at most 300 events):\n";
    try {
        const auto path = "/v1/events?limit=100&type=" + query_value(type) +
                          "&created%5Bgte%5D=" + std::to_string(since_unix);
        std::string cursor;
        std::size_t scanned = 0;
        for (int page_number = 0; page_number < 3; ++page_number) {
            const auto page =
                get(path + (cursor.empty() ? "" : "&starting_after=" + query_value(cursor)));
            if (!page.isObject() || !page["data"].isArray() || !page["has_more"].isBool() ||
                page["data"].size() > 100)
                throw std::runtime_error("Stripe returned a malformed event history page");
            bool found = false;
            for (const auto& event : page["data"]) {
                if (!event.isObject() || !event["id"].isString() ||
                    event["id"].asString().empty() || !event["type"].isString() ||
                    !event["created"].isInt64() || !event["data"]["object"].isObject())
                    throw std::runtime_error("Stripe returned a malformed history event");
                ++scanned;
                if (event["type"].asString() == type &&
                    (must_contain.empty() ||
                     event.toStyledString().find(must_contain) != std::string::npos)) {
                    report += "Found " + event["type"].asString() + " (" + event["id"].asString() +
                              "), created=" + std::to_string(event["created"].asInt64()) + "\n";
                    found = true;
                }
            }
            if (found)
                return report +
                       "Stripe recorded the expected event, but the webhook wait timed out.";
            if (!page["has_more"].asBool())
                return report + "No matching event found in " + std::to_string(scanned) +
                       " events. Stripe may not have recorded it yet.";
            if (page["data"].empty())
                throw std::runtime_error("Stripe returned an empty event page with has_more=true");
            const auto next_cursor = page["data"][page["data"].size() - 1]["id"].asString();
            if (next_cursor == cursor)
                throw std::runtime_error("Stripe event history pagination did not advance");
            cursor = next_cursor;
        }
        return report + "Search incomplete: page limit reached without a matching event.";
    } catch (const std::exception& error) {
        return report + "Event history lookup failed: " + error.what();
    }
}

} // namespace stripekit::testing
