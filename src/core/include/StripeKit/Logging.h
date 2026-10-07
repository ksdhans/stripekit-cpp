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
 * @addtogroup logging
 * @{
 */

/**
 * Severity of a log entry.
 */
enum class LogLevel {
    Trace, /**< Very detailed tracing. Noisy. */
    Debug, /**< Diagnostic detail useful while developing. */
    Info,  /**< Normal, noteworthy activity. */
    Warn,  /**< Something unexpected that did not stop the operation. */
    Error, /**< An operation failed. */
    Off    /**< Nothing at all. Use as a threshold to silence logging. */
};

/**
 * Extra identifiers attached to a log entry.
 *
 * These are the fields worth searching on when tracing a payment through
 * Stripe's dashboard and the application's own logs. A logger that supports
 * structured output should emit them as separate fields.
 *
 * Fields that do not apply are left empty.
 */
struct LogContext {
    /** Stripe request id for an outbound call, of the form "req_...". */
    std::string request_id;

    /** Stripe event id for webhook work, of the form "evt_...". */
    std::string event_id;

    /** Stripe event type, for example "invoice.paid". */
    std::string webhook_type;

    /** Which attempt this is, for operations that are retried. */
    std::optional<int> retry_attempt;

    /** Name of the adapter that produced the entry. */
    std::string adapter_name;
};

/**
 * Receives StripeKit's log entries.
 *
 * Implement this to route StripeKit's diagnostics into the application's own
 * log. Adapters usually provide one already, such as
 * stripekit::drogon::LogBridge.
 *
 * Entries may be written from any thread, so implementations must be
 * thread-safe. They must not throw, and must never write the API key or the
 * webhook signing secret; redact_sensitive_text() helps with the latter.
 *
 * @sa @ref writing_an_adapter
 */
class Logger {
public:
    /**
     * Destroys the logger instance.
     */
    virtual ~Logger() = default;

    /**
     * Reports whether entries at this level are wanted.
     *
     * StripeKit checks this before building a message, so answering honestly
     * avoids formatting work for entries that would be thrown away.
     *
     * @param level Severity being considered.
     * @return True when an entry at that level would be written.
     */
    virtual bool should_log(LogLevel level) const = 0;

    /**
     * Writes one entry.
     *
     * @param level Severity of the entry.
     * @param message The text to record.
     * @param context Identifiers to attach. Fields that do not apply are empty.
     */
    virtual void
    log(LogLevel level, std::string_view message, const LogContext& context = LogContext{}) = 0;
};

/**
 * A logger that discards everything.
 *
 * Use it where a Logger is required but no output is wanted.
 */
class NullLogger final : public Logger {
public:
    /**
     * Reports whether entries at this level are wanted.
     *
     * @param level Ignored.
     * @return Always false.
     */
    bool should_log(LogLevel level) const override;

    /**
     * Discards the entry.
     *
     * @param level Ignored.
     * @param message Ignored.
     * @param context Ignored.
     */
    void log(LogLevel level, std::string_view message, const LogContext& context) override;
};

/**
 * A logger that writes to the process's standard error stream.
 *
 * Convenient for command-line tools and getting started. Applications with
 * their own logging should implement Logger, or use their adapter's bridge.
 */
class ConsoleLogger final : public Logger {
public:
    /**
     * Creates a console logger.
     *
     * @param threshold Lowest severity to write. Entries below it are dropped.
     */
    explicit ConsoleLogger(LogLevel threshold = LogLevel::Info);

    /**
     * Reports whether entries at this level are wanted.
     *
     * @param level Severity being considered.
     * @return True when it is at or above the configured threshold.
     */
    bool should_log(LogLevel level) const override;

    /**
     * Writes one formatted line.
     *
     * @param level Severity of the entry.
     * @param message The text to record.
     * @param context Identifiers to include in the line.
     */
    void log(LogLevel level, std::string_view message, const LogContext& context) override;

private:
    LogLevel threshold_level;
};

/**
 * Reports whether a field is one whose value should never be logged.
 *
 * Recognises the names Stripe uses for keys, secrets, and card details.
 *
 * @param field_name Name of the field being considered.
 * @return True when the value should be replaced before logging.
 */
bool is_sensitive_field_name(std::string_view field_name);

/**
 * Replaces a secret value with a placeholder.
 *
 * @param value The value to hide.
 * @return A placeholder that shows something was present without revealing it.
 */
std::string redact_sensitive_value(std::string_view value);

/**
 * Strips secret-looking tokens out of free-form text.
 *
 * Catches API keys and signing secrets by their recognisable prefixes, which
 * makes it useful for sanitising an error message or a response body before
 * writing it to a log.
 *
 * @param text The text to sanitise.
 * @return The text with recognised secrets replaced by placeholders.
 */
std::string redact_sensitive_text(std::string_view text);

/** @} */

} // namespace stripekit
