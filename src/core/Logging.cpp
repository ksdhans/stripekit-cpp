/** Logging.cpp
 * 
 * See header file for details.
 */

#include "StripeKit/Logging.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>

namespace stripekit {

/** Returns an order index used for threshold comparisons. */
static int log_level_rank(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return 0;
    case LogLevel::Debug:
        return 1;
    case LogLevel::Info:
        return 2;
    case LogLevel::Warn:
        return 3;
    case LogLevel::Error:
        return 4;
    case LogLevel::Off:
        return 5;
    }

    return 5;
}

/** Converts a log level to an uppercase label. */
static const char* log_level_label(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Off:
        return "OFF";
    }

    return "OFF";
}

/** Converts a field name to lowercase ASCII for matching. */
static std::string lowercase_ascii(std::string_view text) {
    std::string output;
    output.reserve(text.size());

    for (char ch : text) {
        output.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    return output;
}

bool NullLogger::should_log(LogLevel level) const {
    (void)level;
    return false;
}

void NullLogger::log(LogLevel level, std::string_view message, const LogContext& context) {
    (void)level;
    (void)message;
    (void)context;
}

ConsoleLogger::ConsoleLogger(LogLevel threshold)
    : threshold_level(threshold) {
}

bool ConsoleLogger::should_log(LogLevel level) const {
    return log_level_rank(level) >= log_level_rank(threshold_level) &&
           threshold_level != LogLevel::Off;
}

void ConsoleLogger::log(LogLevel level, std::string_view message, const LogContext& context) {
    if (!should_log(level)) {
        return;
    }

    std::ostringstream out;
    out << "[StripeKit]";
    out << " [" << log_level_label(level) << "] ";
    out << message;

    if (!context.request_id.empty()) {
        out << " request_id=" << redact_sensitive_value(context.request_id);
    }

    if (!context.event_id.empty()) {
        out << " event_id=" << context.event_id;
    }

    if (!context.webhook_type.empty()) {
        out << " webhook_type=" << context.webhook_type;
    }

    if (context.retry_attempt.has_value()) {
        out << " retry_attempt=" << *context.retry_attempt;
    }

    if (!context.adapter_name.empty()) {
        out << " adapter=" << context.adapter_name;
    }

    std::clog << out.str() << '\n';
}

bool is_sensitive_field_name(std::string_view field_name) {
    const std::string name = lowercase_ascii(field_name);

    return name.find("secret") != std::string::npos ||
           name.find("api_key") != std::string::npos ||
           name.find("authorization") != std::string::npos ||
           name.find("card") != std::string::npos ||
           name.find("bank") != std::string::npos ||
           name.find("token") != std::string::npos;
}

std::string redact_sensitive_value(std::string_view value) {
    if (value.empty()) {
        return "<redacted-empty>";
    }

    return "<redacted>";
}

} // namespace stripekit
