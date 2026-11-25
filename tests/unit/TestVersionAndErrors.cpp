/** TestVersionAndErrors.cpp
 *
 * Unit tests for version helpers, exception types, callbacks, and coroutine tasks.
 */

#include "StripeKit/Async.h"
#include "StripeKit/Callback.h"
#include "StripeKit/Errors.h"
#include "StripeKit/StripeKit.h"
#include "TestFramework.h"

#include <coroutine>
#include <exception>
#include <string>

using namespace stripekit;

TEST(TestFrameworkTest, SuiteSelectionIsExactAndSubstringFilteringStillWorks) {
    stripekit::testing::TestRegistry registry;
    int first = 0;
    int second = 0;
    registry.add_test("BillingTest", "First", [&] { ++first; });
    registry.add_test("OtherBillingTest", "Second", [&] { ++second; });

    EXPECT_TRUE(registry.has_matching_tests("BillingTest", true));
    EXPECT_FALSE(registry.has_matching_tests("Billing", true));
    EXPECT_TRUE(registry.has_matching_tests("Second"));
    EXPECT_FALSE(registry.has_matching_tests("Missing"));
    EXPECT_EQ(registry.run_all("BillingTest", true), 0);
    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 0);
    EXPECT_EQ(registry.run_all("Second"), 0);
    EXPECT_EQ(first, 1);
    EXPECT_EQ(second, 1);
}

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
    StripeErrorPayload payload{ .type = StripeErrorType::RateLimit,
                                .code = "rate_limit",
                                .message = "Too many requests",
                                .http_status = 429,
                                .request_id = "req_123",
                                .retryable = true };
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
    OnceCallback<int> once_cb([&](int value) { count += value; });

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
    EXPECT_THROW({ task.result(); }, std::runtime_error);
}

TEST(AsyncAndCallbackTest, ReadyStateIsFalseBeforeResultAndTrueAfter) {
    auto task = make_simple_task(21);
    EXPECT_FALSE(task.is_ready());
    EXPECT_EQ(task.result(), 42);
    EXPECT_TRUE(task.is_ready());
}

namespace {

/** An awaitable that suspends until the test resumes the coroutine by hand. */
struct ManualSuspension {
    std::coroutine_handle<> handle;

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> awaiting) noexcept {
        handle = awaiting;
    }

    void await_resume() const noexcept {}
};

/** A minimal coroutine whose lifetime the test controls directly. */
struct ManualCoroutine {
    struct promise_type {
        ManualCoroutine get_return_object() {
            return ManualCoroutine{ std::coroutine_handle<promise_type>::from_promise(*this) };
        }

        std::suspend_always initial_suspend() noexcept {
            return {};
        }

        std::suspend_always final_suspend() noexcept {
            return {};
        }

        void return_void() noexcept {}

        void unhandled_exception() {
            std::terminate();
        }
    };

    std::coroutine_handle<promise_type> handle;
};

/** Awaits a task the test owns and records whether the wait completed. */
ManualCoroutine wait_on_task(Task<int>& task, bool& resumed) {
    (void)co_await task;
    resumed = true;
}

/** Suspends until the test releases it, then produces a value. */
Task<int> suspend_then_return(ManualSuspension& suspension, int value) {
    co_await suspension;
    co_return value;
}

/** Awaits a task through an lvalue rather than a temporary. */
Task<int> await_lvalue_task(Task<int>& task) {
    co_return co_await task;
}

} // namespace

TEST(AsyncAndCallbackTest, DestroyingAnAwaitingCoroutineCancelsResumption) {
    ManualSuspension child_suspension;
    Task<int> task = suspend_then_return(child_suspension, 42);

    bool resumed = false;
    auto waiter = wait_on_task(task, resumed);
    waiter.handle.resume();

    // The task is now suspended, and the waiter is suspended on the task.
    EXPECT_FALSE(resumed);
    ASSERT_TRUE(child_suspension.handle);

    // The awaiting coroutine goes away while the task is still in flight.
    waiter.handle.destroy();
    waiter.handle = {};

    // Letting the task finish must not resume the destroyed coroutine.
    child_suspension.handle.resume();
    EXPECT_FALSE(resumed);

    EXPECT_TRUE(task.is_ready());
    EXPECT_EQ(task.result(), 42);
}

TEST(AsyncAndCallbackTest, TaskCanBeAwaitedThroughAnLvalue) {
    auto inner = make_simple_task(21);
    auto outer = await_lvalue_task(inner);
    EXPECT_EQ(outer.result(), 42);
}
