/** JsonValue.h
 *
 * Parses Stripe JSON payloads and reads optional typed values.
 * @author StripeKit contributors
 * @License See LICENSE
 */

#pragma once

#include "StripeKit/Errors.h"

#include <json/json.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace stripekit::json_value {

inline Json::Value parse(std::string_view body) {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;
    if (!reader->parse(body.data(), body.data() + body.size(), &root, &errors)) {
        throw ConfigurationException("Stripe returned invalid JSON: " + errors);
    }
    return root;
}

inline std::string string(const Json::Value& object, const char* name) {
    return object.isMember(name) && object[name].isString() ? object[name].asString() : std::string{};
}

inline std::int64_t integer(const Json::Value& object, const char* name) {
    return object.isMember(name) && object[name].isInt64() ? object[name].asInt64() : 0;
}

inline bool boolean(const Json::Value& object, const char* name) {
    return object.isMember(name) && object[name].isBool() && object[name].asBool();
}

} // namespace stripekit::json_value