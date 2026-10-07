/** TestHelpers.h
 *
 * Test utilities and mock objects for StripeKit unit tests.
 *
 * @author Hans de Ruiter
 *
 * @license See LICENSE.md for details.
 */

#pragma once

#include "StripeKit/Async.h"
#include "StripeKit/Events.h"
#include "StripeKit/HTTPClient.h"
#include "StripeKit/Logging.h"
#include "StripeKit/WebhookDedupStore.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(STRIPEKIT_WITH_LIBSODIUM)
#include <sodium.h>
#else
#error "libsodium is required to build StripeKit test helpers and compute webhook HMAC signatures"
#endif

namespace stripekit::testing {

/**
 * Generates a lowercase hexadecimal string for the given byte buffer.
 */
inline std::string hex_encode(const unsigned char* data, std::size_t size) {
    static constexpr char hex_digits[] = "0123456789abcdef";
    std::string output;
    output.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        output[(i * 2)] = hex_digits[(data[i] >> 4) & 0x0F];
        output[(i * 2) + 1] = hex_digits[data[i] & 0x0F];
    }
    return output;
}

/**
 * Computes the HMAC-SHA256 signature for a signed webhook payload using libsodium.
 */
inline std::string compute_hmac_sha256(std::string_view payload, std::string_view secret) {
    if (sodium_init() < 0) {
        throw std::runtime_error("sodium_init failed in compute_hmac_sha256");
    }
    crypto_auth_hmacsha256_state state;
    crypto_auth_hmacsha256_init(
        &state, reinterpret_cast<const unsigned char*>(secret.data()), secret.size());
    crypto_auth_hmacsha256_update(
        &state, reinterpret_cast<const unsigned char*>(payload.data()), payload.size());

    unsigned char digest[crypto_auth_hmacsha256_BYTES] = { 0 };
    crypto_auth_hmacsha256_final(&state, digest);
    return hex_encode(digest, crypto_auth_hmacsha256_BYTES);
}

/**
 * Constructs a valid Stripe-Signature header value for a given timestamp, payload, and secret.
 */
inline std::string make_stripe_signature_header(std::int64_t timestamp_unix,
                                                std::string_view payload,
                                                std::string_view secret) {
    std::ostringstream signed_payload;
    signed_payload << timestamp_unix << '.' << payload;
    const std::string signature = compute_hmac_sha256(signed_payload.str(), secret);
    return "t=" + std::to_string(timestamp_unix) + ",v1=" + signature;
}

/**
 * In-memory mock HTTPClient that records outgoing requests and returns configured responses.
 */
class MockHttpClient final : public HTTPClient {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    /**
     * Sets a handler function that dynamically generates responses for incoming requests.
     */
    void set_handler(Handler handler_fn) {
        std::lock_guard<std::mutex> lock(mutex);
        handler = std::move(handler_fn);
    }

    /**
     * Sets a constant response to return for all subsequent requests.
     */
    void set_response(HttpResponse response) {
        std::lock_guard<std::mutex> lock(mutex);
        handler = [resp = std::move(response)](const HttpRequest&) { return resp; };
    }

    /**
     * Sets an exception to throw on the next request.
     */
    void set_exception(std::exception_ptr exception) {
        std::lock_guard<std::mutex> lock(mutex);
        next_exception = exception;
    }

    /**
     * Returns the recorded requests in chronological order.
     */
    std::vector<HttpRequest> get_recorded_requests() const {
        std::lock_guard<std::mutex> lock(mutex);
        return recorded_requests;
    }

    /**
     * Clears all recorded requests.
     */
    void clear_recorded_requests() {
        std::lock_guard<std::mutex> lock(mutex);
        recorded_requests.clear();
    }

    Task<HttpResponse> send_async(const HttpRequest& request) override {
        std::unique_lock<std::mutex> lock(mutex);
        recorded_requests.push_back(request);

        if (next_exception) {
            auto ex = next_exception;
            next_exception = nullptr;
            lock.unlock();
            std::rethrow_exception(ex);
        }

        if (handler) {
            auto resp = handler(request);
            lock.unlock();
            co_return resp;
        }

        lock.unlock();
        co_return HttpResponse{ .status_code = 200, .headers = {}, .body = "{}" };
    }

    void send(const HttpRequest& request, Completion completion) override {
        std::unique_lock<std::mutex> lock(mutex);
        recorded_requests.push_back(request);

        if (next_exception) {
            auto ex = next_exception;
            next_exception = nullptr;
            lock.unlock();
            completion(ex);
            return;
        }

        if (handler) {
            try {
                auto resp = handler(request);
                lock.unlock();
                completion(resp);
            } catch (...) {
                lock.unlock();
                completion(std::current_exception());
            }
            return;
        }

        lock.unlock();
        completion(HttpResponse{ .status_code = 200, .headers = {}, .body = "{}" });
    }

private:
    mutable std::mutex mutex;
    std::vector<HttpRequest> recorded_requests;
    Handler handler;
    std::exception_ptr next_exception;
};

/**
 * Mock EventDispatcher for verifying webhook signal dispatching.
 */
class MockEventDispatcher final : public EventDispatcher {
public:
    using DispatchHandler = std::function<EventDispatchResult(const VerifiedEventSignal&)>;

    /**
     * Sets a handler function to produce custom dispatch results.
     */
    void set_handler(DispatchHandler handler_fn) {
        std::lock_guard<std::mutex> lock(mutex);
        handler = std::move(handler_fn);
    }

    /**
     * Sets a static result to return for all dispatches.
     */
    void set_result(EventDispatchResult result) {
        std::lock_guard<std::mutex> lock(mutex);
        default_result = result;
    }

    /**
     * Returns the recorded dispatched signals.
     */
    std::vector<VerifiedEventSignal> get_recorded_signals() const {
        std::lock_guard<std::mutex> lock(mutex);
        return recorded_signals;
    }

    EventDispatchResult dispatch(const VerifiedEventSignal& signal) override {
        std::lock_guard<std::mutex> lock(mutex);
        recorded_signals.push_back(signal);
        if (handler) {
            return handler(signal);
        }
        return default_result;
    }

private:
    mutable std::mutex mutex;
    std::vector<VerifiedEventSignal> recorded_signals;
    EventDispatchResult default_result = EventDispatchResult::Handled;
    DispatchHandler handler;
};

/**
 * Test logger that collects log entries in memory for inspection.
 */
class TestLogger final : public Logger {
public:
    struct Entry {
        LogLevel level;
        std::string message;
        LogContext context;
    };

    explicit TestLogger(LogLevel threshold = LogLevel::Trace) : threshold_level(threshold) {}

    bool should_log(LogLevel level) const override {
        auto rank = [](LogLevel lvl) {
            switch (lvl) {
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
        };
        return rank(level) >= rank(threshold_level) && threshold_level != LogLevel::Off;
    }

    void log(LogLevel level, std::string_view message, const LogContext& context) override {
        if (!should_log(level)) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        entries.push_back(
            Entry{ .level = level, .message = std::string(message), .context = context });
    }

    std::vector<Entry> get_entries() const {
        std::lock_guard<std::mutex> lock(mutex);
        return entries;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        entries.clear();
    }

private:
    LogLevel threshold_level;
    mutable std::mutex mutex;
    std::vector<Entry> entries;
};

} // namespace stripekit::testing
