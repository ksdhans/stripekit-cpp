/** WebhookVerifier.cpp
 *
 * See header file for details.
 */

#include "StripeKit/WebhookVerifier.h"

#include "StripeKit/Errors.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#if defined(STRIPEKIT_WITH_LIBSODIUM)
#include <sodium.h>
#endif

namespace stripekit {

/** Returns a trimmed copy of a token from the signature header. */
static std::string trim_copy(std::string_view value) {
    std::size_t start = 0;
    std::size_t end = value.size();

    while (start < end && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(start, end - start));
}

/** Splits a string by delimiter and returns all segments. */
static std::vector<std::string> split(std::string_view value, char delimiter) {
    std::vector<std::string> parts;

    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t next = value.find(delimiter, start);
        if (next == std::string_view::npos) {
            parts.emplace_back(value.substr(start));
            break;
        }

        parts.emplace_back(value.substr(start, next - start));
        start = next + 1;
    }

    return parts;
}

/** Parses Stripe-Signature header values into timestamp and v1 signatures. */
static void parse_signature_header(std::string_view header,
                                   std::int64_t& timestamp_out,
                                   std::vector<std::string>& v1_signatures_out) {
    bool has_timestamp = false;

    const auto parts = split(header, ',');
    for (const std::string& part : parts) {
        const std::size_t eq = part.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        const std::string key = trim_copy(std::string_view(part).substr(0, eq));
        const std::string value = trim_copy(std::string_view(part).substr(eq + 1));

        if (key == "t") {
            const auto parsed =
                std::from_chars(value.data(), value.data() + value.size(), timestamp_out);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                throw SignatureVerificationException(
                    "Stripe-Signature header contains invalid timestamp");
            }
            has_timestamp = true;
            continue;
        }

        if (key == "v1") {
            v1_signatures_out.push_back(value);
        }
    }

    if (!has_timestamp) {
        throw SignatureVerificationException("Stripe-Signature header is missing timestamp field");
    }

    if (v1_signatures_out.empty()) {
        throw SignatureVerificationException(
            "Stripe-Signature header is missing v1 signature fields");
    }
}

/** Returns lowercase hexadecimal output for a binary buffer. */
static std::string hex_encode(const unsigned char* data, std::size_t size) {
    static constexpr char hex_digits[] = "0123456789abcdef";

    std::string output;
    output.resize(size * 2);

    for (std::size_t i = 0; i < size; ++i) {
        output[(i * 2)] = hex_digits[(data[i] >> 4) & 0x0F];
        output[(i * 2) + 1] = hex_digits[data[i] & 0x0F];
    }

    return output;
}

/** Returns true if signatures are equal in constant time. */
static bool secure_equals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }

#if defined(STRIPEKIT_WITH_LIBSODIUM)
    return sodium_memcmp(left.data(), right.data(), left.size()) == 0;
#else
    volatile unsigned char diff = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return diff == 0;
#endif
}

VerificationMetadata
DefaultWebhookSignatureVerifier::verify(const WebhookVerificationInput& input) const {
#if !defined(STRIPEKIT_WITH_LIBSODIUM)
    (void)input;
    throw ConfigurationException("StripeKit was built without libsodium support");
#else
    if (!is_available()) {
        throw ConfigurationException("Failed to initialize libsodium");
    }

    if (input.payload.empty()) {
        throw SignatureVerificationException("Webhook payload is empty");
    }

    if (input.endpoint_secret.empty()) {
        throw SignatureVerificationException("Webhook endpoint secret is empty");
    }

    std::int64_t timestamp = 0;
    std::vector<std::string> signatures;
    parse_signature_header(input.stripe_signature_header, timestamp, signatures);

    const auto now_unix =
        std::chrono::duration_cast<std::chrono::seconds>(input.now.time_since_epoch()).count();
    const auto age_seconds =
        timestamp >= now_unix
            ? static_cast<std::uint64_t>(timestamp) - static_cast<std::uint64_t>(now_unix)
            : static_cast<std::uint64_t>(now_unix) - static_cast<std::uint64_t>(timestamp);
    const auto tolerance_seconds = input.tolerance.count();
    if (tolerance_seconds < 0 || age_seconds > static_cast<std::uint64_t>(tolerance_seconds)) {
        throw SignatureVerificationException(
            "Webhook signature timestamp is outside allowed tolerance");
    }

    std::ostringstream signed_payload_stream;
    signed_payload_stream << timestamp << '.' << input.payload;
    const std::string signed_payload = signed_payload_stream.str();

    crypto_auth_hmacsha256_state state;
    crypto_auth_hmacsha256_init(
        &state,
        reinterpret_cast<const unsigned char*>(input.endpoint_secret.data()),
        input.endpoint_secret.size());
    crypto_auth_hmacsha256_update(&state,
                                  reinterpret_cast<const unsigned char*>(signed_payload.data()),
                                  signed_payload.size());

    unsigned char digest[crypto_auth_hmacsha256_BYTES] = { 0 };
    crypto_auth_hmacsha256_final(&state, digest);

    const std::string expected_signature = hex_encode(digest, crypto_auth_hmacsha256_BYTES);

    const bool matched = std::any_of(
        signatures.begin(), signatures.end(), [&expected_signature](const std::string& candidate) {
            return secure_equals(candidate, expected_signature);
        });

    if (!matched) {
        throw SignatureVerificationException("Webhook signature does not match");
    }

    return VerificationMetadata{ .timestamp_unix = timestamp, .signed_payload = signed_payload };
#endif
}

bool DefaultWebhookSignatureVerifier::is_available() {
#if defined(STRIPEKIT_WITH_LIBSODIUM)
    return sodium_init() >= 0;
#else
    return false;
#endif
}

} // namespace stripekit
