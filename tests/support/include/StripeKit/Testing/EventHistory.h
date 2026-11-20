/** EventHistory.h
 *
 * Bounded Stripe event-history diagnostics for missing sandbox webhooks.
 *
 * @author StripeKit contributors
 * @License See LICENSE for details.
 */
#pragma once

#include <json/json.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace stripekit::testing {

/**
 * Describe whether Stripe recorded an expected event since the given Unix time.
 *
 * The GET callback must return decoded successful responses or throw on failure.
 * Searches at most three pages of 100 events. Reports incomplete searches and errors
 * explicitly, without returning webhook payloads or changing the result of a webhook wait.
 */
std::string event_history_diagnostics(const std::function<Json::Value(std::string_view)>& get,
                                      std::string_view type,
                                      std::string_view must_contain,
                                      std::int64_t since_unix);

} // namespace stripekit::testing
