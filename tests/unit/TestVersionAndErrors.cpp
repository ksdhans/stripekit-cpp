/** TestVersionAndErrors.cpp
 * 
 * Unit tests for version helpers, exception types, callbacks, and coroutine tasks.
 */

#include "TestFramework.h"
#include "StripeKit/Async.h"
#include "StripeKit/Callback.h"
#include "StripeKit/Errors.h"
#include "StripeKit/StripeKit.h"

#include <string>

using namespace stripekit;

TEST(VersionAndErrorsTest, VersionFunctionsReturnNonEmpty) {
    EXPECT_FALSE(version().empty());
    EXPECT_GE(version_major(), 0);
    EXPECT_GE(version_minor(), 0);
    EXPECT_GE(version_patch(), 0);
    EXPECT_FALSE(build_id().empty());
    EXPECT_FALSE(build_date().empty());
}

TEST(VersionAndErrorsTest, ExceptionHierarchy) {
    // Base exception
    StripeKitException base_ex("Generic stripekit error");
    EXPECT_STREQ(base_ex.what(), "Generic stripekit error");

    // Signature verification exception
    SignatureVerificationException sig_ex("Bad signature");
    EXPECT_STREQ(sig_ex.what(), "Bad signature");
    EXPECT_NE(dynamic_cast<const StripeKitException*>(&sig_ex), nullptr);

    // Configuration exception
    ConfigurationException conf_ex("Invalid config");
    EXPECT_STREQ(conf_ex.what(), "Invalid config");
    EXPECT_NE(dynamic_cast<const StripeKitException*>(&conf_ex), nullptr);

    // API exception
    StripeErrorPayload payload{
        .type = StripeErrorType::RateLimit,
        .code = "rate_limit",
        .message = "Too many requests",
        .http_status = 429,
        .request_id = "req_123",
        .retryable = true
    };
    StripeApiException api_ex(payload);
    EXPECT_STREQ(api_ex.what(), "Too many requests");
    EXPECT_EQ(api_ex.payload().type, StripeErrorType::RateLimit);
    EXPECT_EQ(api_ex.payload().code, "rate_limit");
    EXPECT_EQ(api_ex.payload().http_status, 429);
    EXPECT_TRUE(api_ex.retryable());
    EXPECT_NE(dynamic_cast<const StripeKitException*>(&api_ex), nullptr);
}

TEST(AsyncAndCallbackTest, OnceCallbackGuaranteesExactlyOnceExecution) {
    int count = 0;
    OnceCallback<int> once_cb([&](int value) {
        count += value;
    });

    EXPECT_FALSE(once_cb.was_invoked());

    // First invocation
    EXPECT_TRUE(once_cb.invoke(42));
    EXPECT_EQ(count, 42);
    EXPECT_TRUE(once_cb.was_invoked());

    // Second invocation is ignored
    EXPECT_FALSE(once_cb.invoke(100));
    EXPECT_EQ(count, 42);
}

static Task<int> make_simple_task(int value) {
    co_return value * 2;
}

static Task<int> make_failing_task() {
    throw std::runtime_error("coroutine error");
    co_return 0;
}

static Task<int> make_composed_task() {
    int a = co_await make_simple_task(5);
    int b = co_await make_simple_task(10);
    co_return a + b;
}

TEST(AsyncAndCallbackTest, TaskSynchronousResult) {
    auto task = make_simple_task(21);
    EXPECT_EQ(task.result(), 42);
}

TEST(AsyncAndCallbackTest, TaskCompositionWithCoAwait) {
    auto task = make_composed_task();
    EXPECT_EQ(task.result(), 30);
}

TEST(AsyncAndCallbackTest, TaskExceptionPropagation) {
    auto task = make_failing_task();
    EXPECT_THROW({
        task.result();
    }, std::runtime_error);
}
