/** Errors.cpp
 * 
 * See header file for details.
 */

#include "StripeKit/Errors.h"

#include <utility>

namespace stripekit {

StripeKitException::StripeKitException(const std::string& message)
    : std::runtime_error(message) {
}

StripeKitException::~StripeKitException() = default;

StripeApiException::StripeApiException(StripeErrorPayload payload)
    : StripeKitException(payload.message), payload_data(std::move(payload)) {
}

const StripeErrorPayload& StripeApiException::payload() const noexcept {
    return payload_data;
}

bool StripeApiException::retryable() const noexcept {
    return payload_data.retryable;
}

SignatureVerificationException::SignatureVerificationException(const std::string& message)
    : StripeKitException(message) {
}

SignatureVerificationException::~SignatureVerificationException() = default;

ConfigurationException::ConfigurationException(const std::string& message)
    : StripeKitException(message) {
}

ConfigurationException::~ConfigurationException() = default;

} // namespace stripekit
