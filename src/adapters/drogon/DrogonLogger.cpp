/** DrogonLogger.cpp
 *
 * See header file for details.
 */

#include "StripeKit/Adapters/Drogon.h"

#include <drogon/drogon.h>

#include <string>

namespace stripekit {
namespace drogon {
namespace {

int log_rank(LogLevel level) {
    return static_cast<int>(level);
}

std::string log_message(std::string_view message, const LogContext& context) {
    std::string output(message);
    if (!context.event_id.empty()) {
        output += " event_id=";
        output += context.event_id;
    }
    if (!context.webhook_type.empty()) {
        output += " webhook_type=";
        output += context.webhook_type;
    }
    if (context.retry_attempt.has_value()) {
        output += " retry_attempt=";
        output += std::to_string(*context.retry_attempt);
    }
    if (!context.adapter_name.empty()) {
        output += " adapter=";
        output += context.adapter_name;
    }
    return output;
}

} // namespace

LogBridge::LogBridge(LogLevel threshold) : threshold(threshold) {}

bool LogBridge::should_log(LogLevel level) const {
    return threshold != LogLevel::Off && log_rank(level) >= log_rank(threshold);
}

void LogBridge::log(LogLevel level, std::string_view message, const LogContext& context) {
    if (!should_log(level)) {
        return;
    }

    // Redact here as well as at call sites: callers may forget, and an
    // unredacted API key or signing secret must not reach Drogon's log.
    const auto formatted = log_message(redact_sensitive_text(message), context);
    switch (level) {
    case LogLevel::Trace:
        LOG_TRACE << formatted;
        break;
    case LogLevel::Debug:
        LOG_DEBUG << formatted;
        break;
    case LogLevel::Info:
        LOG_INFO << formatted;
        break;
    case LogLevel::Warn:
        LOG_WARN << formatted;
        break;
    case LogLevel::Error:
        LOG_ERROR << formatted;
        break;
    case LogLevel::Off:
        break;
    }
}

} // namespace drogon
} // namespace stripekit