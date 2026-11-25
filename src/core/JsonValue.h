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
#include <exception>
#include <memory>
#include <string>
#include <string_view>

namespace stripekit::json_value {

/**
 * Parses a JSON document, reporting any failure as a ConfigurationException.
 *
 * JsonCpp can throw rather than report a failure through its return value, for
 * example when its nesting limit is exceeded. Those exceptions are converted so
 * callers only ever see StripeKit's own exception types.
 *
 * @param body The JSON text to parse.
 * @return The parsed root value.
 * @throws ConfigurationException The text is not valid JSON, or JsonCpp could
 *         not parse it.
 */
inline Json::Value parse(std::string_view body) {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;
    bool parsed = false;
    try {
        parsed = reader->parse(body.data(), body.data() + body.size(), &root, &errors);
    } catch (const std::exception& error) {
        throw ConfigurationException(std::string("Stripe returned invalid JSON: ") + error.what());
    }
    if (!parsed) {
        throw ConfigurationException("Stripe returned invalid JSON: " + errors);
    }
    return root;
}

inline std::string string(const Json::Value& object, const char* name) {
    return object.isObject() && object.isMember(name) && object[name].isString()
               ? object[name].asString()
               : std::string{};
}

inline std::int64_t integer(const Json::Value& object, const char* name) {
    return object.isObject() && object.isMember(name) && object[name].isInt64()
               ? object[name].asInt64()
               : 0;
}

inline bool boolean(const Json::Value& object, const char* name) {
    return object.isObject() && object.isMember(name) && object[name].isBool() &&
           object[name].asBool();
}

} // namespace stripekit::json_value