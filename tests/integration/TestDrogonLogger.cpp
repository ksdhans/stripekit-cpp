/** TestDrogonLogger.cpp
 *
 * Integration tests for Drogon logger bridge, log level threshold filtering,
 * and secret redaction.
 */

#include "TestFramework.h"

#include "StripeKit/Adapters/Drogon.h"
#include "StripeKit/Logging.h"

#include <trantor/utils/Logger.h>

#include <cstdio>
#include <string>

using namespace stripekit;

TEST(DrogonLoggerTest, LogBridgeRespectsThresholdLevels) {
    stripekit::drogon::LogBridge info_logger(LogLevel::Info);
    EXPECT_FALSE(info_logger.should_log(LogLevel::Trace));
    EXPECT_FALSE(info_logger.should_log(LogLevel::Debug));
    EXPECT_TRUE(info_logger.should_log(LogLevel::Info));
    EXPECT_TRUE(info_logger.should_log(LogLevel::Warn));
    EXPECT_TRUE(info_logger.should_log(LogLevel::Error));

    stripekit::drogon::LogBridge off_logger(LogLevel::Off);
    EXPECT_FALSE(off_logger.should_log(LogLevel::Trace));
    EXPECT_FALSE(off_logger.should_log(LogLevel::Debug));
    EXPECT_FALSE(off_logger.should_log(LogLevel::Info));
    EXPECT_FALSE(off_logger.should_log(LogLevel::Warn));
    EXPECT_FALSE(off_logger.should_log(LogLevel::Error));
    EXPECT_FALSE(off_logger.should_log(LogLevel::Off));
}

TEST(DrogonLoggerTest, LogBridgeEmitsFormattedLogEntriesWithoutCrashing) {
    stripekit::drogon::LogBridge logger(LogLevel::Trace);

    LogContext context{ .request_id = "req_log_test_1",
                        .event_id = "evt_log_test_2",
                        .webhook_type = "invoice.paid",
                        .retry_attempt = 1,
                        .adapter_name = "drogon" };

    EXPECT_NO_THROW({
        logger.log(LogLevel::Info, "Testing integration log message", context);
        logger.log(LogLevel::Warn, "Warning message with context", context);
        logger.log(LogLevel::Error, "Error message with context", context);
        logger.log(LogLevel::Debug, "Debug message with context", context);
        logger.log(LogLevel::Trace, "Trace message with context", context);
    });
}

TEST(DrogonLoggerTest, LogBridgeRedactsSecretsFromMessages) {
    const auto previous_level = trantor::Logger::logLevel();
    std::string captured;
    trantor::Logger::setOutputFunction(
        [&captured](const char* msg, const uint64_t len) { captured.append(msg, len); }, []() {});
    trantor::Logger::setLogLevel(trantor::Logger::kTrace);

    stripekit::drogon::LogBridge logger(LogLevel::Trace);
    logger.log(LogLevel::Error,
               "charge failed for sk_live_1234abcd and webhook secret whsec_supersecret9",
               LogContext{ .event_id = "evt_log_redact_test" });

    // Restore the stdout writer and level so later tests are unaffected.
    trantor::Logger::setOutputFunction(
        [](const char* msg, const uint64_t len) {
            std::fwrite(msg, 1, static_cast<std::size_t>(len), stdout);
        },
        []() { std::fflush(stdout); });
    trantor::Logger::setLogLevel(previous_level);

    EXPECT_EQ(captured.find("sk_live_1234abcd"), std::string::npos);
    EXPECT_EQ(captured.find("whsec_supersecret9"), std::string::npos);
    EXPECT_NE(captured.find("sk_live_"), std::string::npos);
    EXPECT_NE(captured.find("evt_log_redact_test"), std::string::npos);
}
