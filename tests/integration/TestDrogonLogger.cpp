/** TestDrogonLogger.cpp
 *
 * Integration tests for Drogon logger bridge and log level threshold filtering.
 */

#include "TestFramework.h"

#include "StripeKit/Adapters/Drogon.h"
#include "StripeKit/Logging.h"

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
