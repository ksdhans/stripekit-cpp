/** DrogonLogger.cpp
 *
 * See header file for details.
 */

#include "StripeKit/Drogon.h"

#include <drogon/drogon.h>

#include <sstream>

namespace stripekit {

namespace {

int log_rank(LogLevel level) {
    return static_cast<int>(level);
}

std::string log_message(std::string_view message, const LogContext& context) {
    std::ostringstream output;
    output << message;
    if (!context.event_id.empty()) {
        output << " event_id=" << context.event_id;
    }
    if (!context.webhook_type.empty()) {
        output << " webhook_type=" << context.webhook_type;
    }
    if (context.retry_attempt.has_value()) {
        output << " retry_attempt=" << *context.retry_attempt;
    }
    if (!context.adapter_name.empty()) {
        output << " adapter=" << context.adapter_name;
    }
    return output.str();
}

} // namespace

DrogonLogger::DrogonLogger(LogLevel threshold) : threshold(threshold) {}

bool DrogonLogger::should_log(LogLevel level) const {
    return threshold != LogLevel::Off && log_rank(level) >= log_rank(threshold);
}

void DrogonLogger::log(LogLevel level, std::string_view message, const LogContext& context) {
    if (!should_log(level)) {
        return;
    }

    const auto formatted = log_message(message, context);
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

} // namespace stripekit