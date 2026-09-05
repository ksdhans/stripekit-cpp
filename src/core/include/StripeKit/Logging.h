/** Logging.h
 * 
 * System for StripeKit logging..
 * 
 * @author Hans de Ruiter
 * 
 * @license See LICENSE.md for details.
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace stripekit {

/**
 * Logging levels supported by StripeKit.
 */
enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Off
};

/**
 * Optional metadata fields that can be attached to a log message.
 *
 * Leave any field empty when it is not relevant for a given message.
 */
struct LogContext {
    /** Stripe request id associated with an outbound request. */
    std::string request_id;

    /** Stripe event id associated with webhook processing. */
    std::string event_id;

    /** Stripe webhook type, for example invoice.paid. */
    std::string webhook_type;

    /** Retry attempt index where available. */
    std::optional<int> retry_attempt;

    /** Adapter name generating the log entry. */
    std::string adapter_name;
};

/**
 * Abstract base class for StripeKit loggers.
 */
class Logger {
public:
    /**
     * Destroys the logger instance.
     */
    virtual ~Logger() = default;

    /**
     * Returns true when entries at the given level should be emitted.
     */
    virtual bool should_log(LogLevel level) const = 0;

    /**
     * Writes a message to the log.
     *
     * Context is optional. Pass an empty context when there is no request or
     * webhook metadata to attach.
     */
    virtual void log(LogLevel level,
                     std::string_view message,
                     const LogContext& context = LogContext{}) = 0;
};

/**
 * No-op logger implementation.
 */
class NullLogger final : public Logger {
public:
    /**
     * Returns false for all levels.
     */
    bool should_log(LogLevel level) const override;

    /**
     * Ignores all log requests.
     */
    void log(LogLevel level, std::string_view message, const LogContext& context) override;
};

/**
 * Console logger fallback used when no framework bridge logger is supplied.
 */
class ConsoleLogger final : public Logger {
public:
    /**
     * Creates a console logger with the provided minimum level threshold.
     */
    explicit ConsoleLogger(LogLevel threshold = LogLevel::Info);

    /**
     * Returns true when level is at or above the configured threshold.
     */
    bool should_log(LogLevel level) const override;

    /**
     * Writes a formatted line to the process log stream.
     */
    void log(LogLevel level, std::string_view message, const LogContext& context) override;

private:
    LogLevel threshold_level;
};

/**
 * Returns true when the field name should be treated as sensitive.
 */
bool is_sensitive_field_name(std::string_view field_name);

/**
 * Returns a redacted placeholder for sensitive values.
 */
std::string redact_sensitive_value(std::string_view value);

/**
 * Redacts secret-looking tokens from free-form log text.
 */
std::string redact_sensitive_text(std::string_view text);

} // namespace stripekit
