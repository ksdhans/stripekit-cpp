/** TestLogging.cpp
 * 
 * Unit tests for StripeKit logging utilities and sensitive data redaction.
 */

#include "StripeKit/Logging.h"

#include <gtest/gtest.h>

#include <sstream>

using namespace stripekit;

TEST(LoggingTest, NullLoggerIgnoresAllLevels) {
    NullLogger logger;
    EXPECT_FALSE(logger.should_log(LogLevel::Trace));
    EXPECT_FALSE(logger.should_log(LogLevel::Debug));
    EXPECT_FALSE(logger.should_log(LogLevel::Info));
    EXPECT_FALSE(logger.should_log(LogLevel::Warn));
    EXPECT_FALSE(logger.should_log(LogLevel::Error));
    EXPECT_FALSE(logger.should_log(LogLevel::Off));

    // Must not throw or crash
    logger.log(LogLevel::Error, "Test message", LogContext{.event_id = "evt_123"});
}

TEST(LoggingTest, ConsoleLoggerRespectsThreshold) {
    ConsoleLogger info_logger(LogLevel::Info);
    EXPECT_FALSE(info_logger.should_log(LogLevel::Trace));
    EXPECT_FALSE(info_logger.should_log(LogLevel::Debug));
    EXPECT_TRUE(info_logger.should_log(LogLevel::Info));
    EXPECT_TRUE(info_logger.should_log(LogLevel::Warn));
    EXPECT_TRUE(info_logger.should_log(LogLevel::Error));

    ConsoleLogger off_logger(LogLevel::Off);
    EXPECT_FALSE(off_logger.should_log(LogLevel::Trace));
    EXPECT_FALSE(off_logger.should_log(LogLevel::Error));
    EXPECT_FALSE(off_logger.should_log(LogLevel::Off));
}

TEST(LoggingTest, SensitiveFieldDetection) {
    EXPECT_TRUE(is_sensitive_field_name("api_key"));
    EXPECT_TRUE(is_sensitive_field_name("stripe_api_key"));
    EXPECT_TRUE(is_sensitive_field_name("Authorization"));
    EXPECT_TRUE(is_sensitive_field_name("endpoint_secret"));
    EXPECT_TRUE(is_sensitive_field_name("client_secret"));
    EXPECT_TRUE(is_sensitive_field_name("card_number"));
    EXPECT_TRUE(is_sensitive_field_name("bank_account"));
    EXPECT_TRUE(is_sensitive_field_name("session_token"));

    EXPECT_FALSE(is_sensitive_field_name("customer_id"));
    EXPECT_FALSE(is_sensitive_field_name("status"));
    EXPECT_FALSE(is_sensitive_field_name("email"));
    EXPECT_FALSE(is_sensitive_field_name("amount"));
    EXPECT_FALSE(is_sensitive_field_name("id"));
}

TEST(LoggingTest, SensitiveValueRedaction) {
    EXPECT_EQ(redact_sensitive_value(""), "<redacted-empty>");
    EXPECT_EQ(redact_sensitive_value("sk_test_1234567890"), "<redacted>");
    EXPECT_EQ(redact_sensitive_value("whsec_secret"), "<redacted>");
}
