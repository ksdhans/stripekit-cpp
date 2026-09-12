/** TestWebhookVerifier.cpp
 * 
 * Unit tests for Stripe webhook signature verification.
 */

#include "TestFramework.h"
#include "TestHelpers.h"

#include "StripeKit/Errors.h"
#include "StripeKit/WebhookVerifier.h"

#include <chrono>
#include <string>

using namespace stripekit;
using namespace stripekit::testing;

class WebhookVerifierTest : public ::testing::Test {
protected:
    void SetUp() override {
        secret = "whsec_test_secret_key_1234567890abcdef";
        payload = R"({"id":"evt_123","type":"invoice.paid","created":1700000000})";
        now = std::chrono::system_clock::from_time_t(1700000000);
        timestamp_unix = 1700000000;
    }

    std::string secret;
    std::string payload;
    std::chrono::system_clock::time_point now;
    std::int64_t timestamp_unix = 0;
    DefaultWebhookSignatureVerifier verifier;
};

TEST_F(WebhookVerifierTest, IsAvailableReturnsTrue) {
    EXPECT_TRUE(DefaultWebhookSignatureVerifier::is_available());
}

TEST_F(WebhookVerifierTest, ValidSignaturePasses) {
    const std::string header = make_stripe_signature_header(timestamp_unix, payload, secret);

    const WebhookVerificationInput input{
        .payload = payload,
        .stripe_signature_header = header,
        .endpoint_secret = secret,
        .now = now,
        .tolerance = std::chrono::minutes(5)
    };

    const VerificationMetadata metadata = verifier.verify(input);
    EXPECT_EQ(metadata.timestamp_unix, timestamp_unix);
    EXPECT_EQ(metadata.signed_payload, std::to_string(timestamp_unix) + "." + payload);
}

TEST_F(WebhookVerifierTest, ValidSignatureWithMultipleSignaturesPasses) {
    const std::string valid_sig = compute_hmac_sha256(std::to_string(timestamp_unix) + "." + payload, secret);
    const std::string dummy_sig = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string header = "t=" + std::to_string(timestamp_unix) + ",v1=" + dummy_sig + ",v1=" + valid_sig;

    const WebhookVerificationInput input{
        .payload = payload,
        .stripe_signature_header = header,
        .endpoint_secret = secret,
        .now = now,
        .tolerance = std::chrono::minutes(5)
    };

    const VerificationMetadata metadata = verifier.verify(input);
    EXPECT_EQ(metadata.timestamp_unix, timestamp_unix);
}

TEST_F(WebhookVerifierTest, TamperedPayloadThrows) {
    const std::string header = make_stripe_signature_header(timestamp_unix, payload, secret);
    const std::string tampered_payload = payload + " ";

    const WebhookVerificationInput input{
        .payload = tampered_payload,
        .stripe_signature_header = header,
        .endpoint_secret = secret,
        .now = now,
        .tolerance = std::chrono::minutes(5)
    };

    EXPECT_THROW({
        verifier.verify(input);
    }, SignatureVerificationException);
}

TEST_F(WebhookVerifierTest, WrongSecretThrows) {
    const std::string header = make_stripe_signature_header(timestamp_unix, payload, secret);
    const std::string wrong_secret = "whsec_wrong_secret_abcdef1234567890";

    const WebhookVerificationInput input{
        .payload = payload,
        .stripe_signature_header = header,
        .endpoint_secret = wrong_secret,
        .now = now,
        .tolerance = std::chrono::minutes(5)
    };

    EXPECT_THROW({
        verifier.verify(input);
    }, SignatureVerificationException);
}

TEST_F(WebhookVerifierTest, MissingTimestampThrows) {
    const std::string dummy_sig = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string header = "v1=" + dummy_sig;

    const WebhookVerificationInput input{
        .payload = payload,
        .stripe_signature_header = header,
        .endpoint_secret = secret,
        .now = now,
        .tolerance = std::chrono::minutes(5)
    };

    EXPECT_THROW({
        verifier.verify(input);
    }, SignatureVerificationException);
}

TEST_F(WebhookVerifierTest, InvalidTimestampThrows) {
    const std::string dummy_sig = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string header = "t=invalid_timestamp,v1=" + dummy_sig;

    const WebhookVerificationInput input{
        .payload = payload,
        .stripe_signature_header = header,
        .endpoint_secret = secret,
        .now = now,
        .tolerance = std::chrono::minutes(5)
    };

    EXPECT_THROW({
        verifier.verify(input);
    }, SignatureVerificationException);
}

TEST_F(WebhookVerifierTest, TimestampWithTrailingDataThrows) {
    const std::string dummy_sig = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string header = "t=1700000000trailing,v1=" + dummy_sig;

    const WebhookVerificationInput input{
        .payload = payload,
        .stripe_signature_header = header,
        .endpoint_secret = secret,
        .now = now,
        .tolerance = std::chrono::minutes(5)
    };

    EXPECT_THROW({
        verifier.verify(input);
    }, SignatureVerificationException);
}

TEST_F(WebhookVerifierTest, ExtremeTimestampDoesNotOverflowAgeCalculation) {
    const std::string dummy_sig = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const std::string header = "t=-9223372036854775808,v1=" + dummy_sig;

    const WebhookVerificationInput input{
        .payload = payload,
        .stripe_signature_header = header,
        .endpoint_secret = secret,
        .now = now,
        .tolerance = std::chrono::minutes(5)
    };

    EXPECT_THROW({
        verifier.verify(input);
    }, SignatureVerificationException);
}

TEST_F(WebhookVerifierTest, MissingV1SignaturesThrows) {
    const std::string header = "t=" + std::to_string(timestamp_unix);

    const WebhookVerificationInput input{
        .payload = payload,
        .stripe_signature_header = header,
        .endpoint_secret = secret,
        .now = now,
        .tolerance = std::chrono::minutes(5)
    };

    EXPECT_THROW({
        verifier.verify(input);
    }, SignatureVerificationException);
}

TEST_F(WebhookVerifierTest, EmptyPayloadThrows) {
    const std::string header = make_stripe_signature_header(timestamp_unix, "", secret);

    const WebhookVerificationInput input{
        .payload = "",
        .stripe_signature_header = header,
        .endpoint_secret = secret,
        .now = now,
        .tolerance = std::chrono::minutes(5)
    };

    EXPECT_THROW({
        verifier.verify(input);
    }, SignatureVerificationException);
}

TEST_F(WebhookVerifierTest, EmptySecretThrows) {
    const std::string header = make_stripe_signature_header(timestamp_unix, payload, "test");

    const WebhookVerificationInput input{
        .payload = payload,
        .stripe_signature_header = header,
        .endpoint_secret = "",
        .now = now,
        .tolerance = std::chrono::minutes(5)
    };

    EXPECT_THROW({
        verifier.verify(input);
    }, SignatureVerificationException);
}

TEST_F(WebhookVerifierTest, TimestampTolerancePassesWithinWindow) {
    // Exactly 300 seconds (5 min) difference
    const std::int64_t past_timestamp = timestamp_unix - 300;
    const std::string header = make_stripe_signature_header(past_timestamp, payload, secret);

    const WebhookVerificationInput input{
        .payload = payload,
        .stripe_signature_header = header,
        .endpoint_secret = secret,
        .now = now,
        .tolerance = std::chrono::seconds(300)
    };

    EXPECT_NO_THROW({
        verifier.verify(input);
    });
}

TEST_F(WebhookVerifierTest, TimestampToleranceFailsOutsideWindow) {
    // 301 seconds difference (past)
    const std::int64_t past_timestamp = timestamp_unix - 301;
    const std::string header = make_stripe_signature_header(past_timestamp, payload, secret);

    const WebhookVerificationInput input{
        .payload = payload,
        .stripe_signature_header = header,
        .endpoint_secret = secret,
        .now = now,
        .tolerance = std::chrono::seconds(300)
    };

    EXPECT_THROW({
        verifier.verify(input);
    }, SignatureVerificationException);
}

TEST_F(WebhookVerifierTest, TimestampToleranceFailsInFutureOutsideWindow) {
    // 301 seconds in future
    const std::int64_t future_timestamp = timestamp_unix + 301;
    const std::string header = make_stripe_signature_header(future_timestamp, payload, secret);

    const WebhookVerificationInput input{
        .payload = payload,
        .stripe_signature_header = header,
        .endpoint_secret = secret,
        .now = now,
        .tolerance = std::chrono::seconds(300)
    };

    EXPECT_THROW({
        verifier.verify(input);
    }, SignatureVerificationException);
}

TEST_F(WebhookVerifierTest, WhitespaceInHeaderIsTolerated) {
    const std::string valid_sig = compute_hmac_sha256(std::to_string(timestamp_unix) + "." + payload, secret);
    const std::string header = "  t = " + std::to_string(timestamp_unix) + " ,  v1 = " + valid_sig + "  ";

    const WebhookVerificationInput input{
        .payload = payload,
        .stripe_signature_header = header,
        .endpoint_secret = secret,
        .now = now,
        .tolerance = std::chrono::minutes(5)
    };

    const VerificationMetadata metadata = verifier.verify(input);
    EXPECT_EQ(metadata.timestamp_unix, timestamp_unix);
}
