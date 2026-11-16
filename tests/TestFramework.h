/** TestFramework.h
 *
 * Lightweight, zero-dependency test runner macros for CTest.
 *
 * @author Hans de Ruiter
 *
 * @license See LICENSE.md for details.
 */

#pragma once

#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace stripekit::testing {

inline void append_test_progress(const std::string& message) {
    const char* path = std::getenv("STRIPEKIT_TEST_PROGRESS_LOG");
    if (path == nullptr || *path == '\0') {
        return;
    }

    std::ofstream log(path, std::ios::app);
    if (log) {
        log << message << '\n';
    }
}

struct TestCase {
    std::string suite_name;
    std::string test_name;
    std::function<void()> test_fn;
};

class TestRegistry {
public:
    static TestRegistry& instance() {
        static TestRegistry registry;
        return registry;
    }

    void add_test(std::string suite_name, std::string test_name, std::function<void()> fn) {
        tests.push_back(TestCase{ .suite_name = std::move(suite_name),
                                  .test_name = std::move(test_name),
                                  .test_fn = std::move(fn) });
    }

    void list_tests() const {
        for (const auto& test : tests) {
            std::cout << test.suite_name << "." << test.test_name << std::endl;
        }
    }

    /** Return whether a filter matches at least one registered test. */
    bool has_matching_tests(const std::string& filter, bool match_suite = false) const {
        for (const auto& test : tests) {
            if (match_suite
                    ? test.suite_name == filter
                    : (test.suite_name + "." + test.test_name).find(filter) != std::string::npos)
                return true;
        }
        return false;
    }

    /** Run matching tests, optionally matching the suite name exactly. */
    int run_all(const std::string& filter = "", bool match_suite = false) {
        int passed = 0;
        int failed = 0;

        for (const auto& test : tests) {
            const std::string full_name = test.suite_name + "." + test.test_name;
            if (match_suite ? test.suite_name != filter
                            : (!filter.empty() && full_name.find(filter) == std::string::npos)) {
                continue;
            }

            const std::string run_message = "[ RUN      ] " + full_name;
            std::cout << run_message << std::endl;
            append_test_progress(run_message);
            bool success = true;
            try {
                test.test_fn();
            } catch (const std::exception& ex) {
                const std::string failure_message =
                    "[  FAILED  ] " + full_name + " threw exception: " + ex.what();
                std::cerr << failure_message << std::endl;
                append_test_progress(failure_message);
                success = false;
            } catch (...) {
                const std::string failure_message =
                    "[  FAILED  ] " + full_name + " threw unknown exception";
                std::cerr << failure_message << std::endl;
                append_test_progress(failure_message);
                success = false;
            }

            if (success) {
                const std::string ok_message = "[       OK ] " + full_name;
                std::cout << ok_message << std::endl;
                append_test_progress(ok_message);
                ++passed;
            } else {
                ++failed;
            }
        }

        std::cout << "\nTest Summary: " << passed << " passed, " << failed << " failed."
                  << std::endl;
        return failed == 0 ? 0 : 1;
    }

private:
    std::vector<TestCase> tests;
};

struct TestRegistrar {
    TestRegistrar(const char* suite_name, const char* test_name, std::function<void()> fn) {
        TestRegistry::instance().add_test(suite_name, test_name, std::move(fn));
    }
};

class TestFailureException : public std::runtime_error {
public:
    explicit TestFailureException(const std::string& msg) : std::runtime_error(msg) {}
};

template <typename T>
concept Streamable = requires(std::ostream& os, const T& value) {
    { os << value } -> std::same_as<std::ostream&>;
};

template <typename T>
std::string format_val(const T& val) {
    if constexpr (Streamable<T>) {
        std::ostringstream ss;
        ss << val;
        return ss.str();
    } else if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<std::underlying_type_t<T>>(val));
    } else {
        return "<unprintable>";
    }
}

#define TEST_CONCAT_INNER(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_INNER(a, b)

#define TEST(suite_name, test_name)                                                                \
    void TEST_CONCAT(test_fn_, TEST_CONCAT(suite_name, TEST_CONCAT(_, test_name)))();              \
    static ::stripekit::testing::TestRegistrar TEST_CONCAT(                                        \
        reg_, TEST_CONCAT(suite_name, TEST_CONCAT(_, test_name)))(                                 \
        #suite_name,                                                                               \
        #test_name,                                                                                \
        TEST_CONCAT(test_fn_, TEST_CONCAT(suite_name, TEST_CONCAT(_, test_name))));                \
    void TEST_CONCAT(test_fn_, TEST_CONCAT(suite_name, TEST_CONCAT(_, test_name)))()

#define TEST_F(fixture_class, test_name)                                                           \
    struct TEST_CONCAT(FixtureTest_, TEST_CONCAT(fixture_class, TEST_CONCAT(_, test_name)))        \
        : public fixture_class {                                                                   \
        void run();                                                                                \
        void execute() {                                                                           \
            SetUp();                                                                               \
            try {                                                                                  \
                run();                                                                             \
                TearDown();                                                                        \
            } catch (...) {                                                                        \
                TearDown();                                                                        \
                throw;                                                                             \
            }                                                                                      \
        }                                                                                          \
    };                                                                                             \
    static ::stripekit::testing::TestRegistrar TEST_CONCAT(                                        \
        reg_fixture_,                                                                              \
        TEST_CONCAT(fixture_class, TEST_CONCAT(_, test_name)))(#fixture_class, #test_name, []() {  \
        TEST_CONCAT(FixtureTest_, TEST_CONCAT(fixture_class, TEST_CONCAT(_, test_name)))           \
        test_inst;                                                                                 \
        test_inst.execute();                                                                       \
    });                                                                                            \
    void TEST_CONCAT(FixtureTest_, TEST_CONCAT(fixture_class, TEST_CONCAT(_, test_name)))::run()

#define EXPECT_TRUE(expr)                                                                          \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::ostringstream ss;                                                                 \
            ss << __FILE__ << ":" << __LINE__ << " Check failed: EXPECT_TRUE(" #expr ")";          \
            std::cerr << ss.str() << std::endl;                                                    \
            throw ::stripekit::testing::TestFailureException(ss.str());                            \
        }                                                                                          \
    } while (0)

#define EXPECT_FALSE(expr)                                                                         \
    do {                                                                                           \
        if (expr) {                                                                                \
            std::ostringstream ss;                                                                 \
            ss << __FILE__ << ":" << __LINE__ << " Check failed: EXPECT_FALSE(" #expr ")";         \
            std::cerr << ss.str() << std::endl;                                                    \
            throw ::stripekit::testing::TestFailureException(ss.str());                            \
        }                                                                                          \
    } while (0)

#define EXPECT_EQ(val1, val2)                                                                      \
    do {                                                                                           \
        if (!((val1) == (val2))) {                                                                 \
            std::ostringstream ss;                                                                 \
            ss << __FILE__ << ":" << __LINE__                                                      \
               << " Check failed: EXPECT_EQ(" #val1 ", " #val2 ") ["                               \
               << ::stripekit::testing::format_val(val1) << " vs "                                 \
               << ::stripekit::testing::format_val(val2) << "]";                                   \
            std::cerr << ss.str() << std::endl;                                                    \
            throw ::stripekit::testing::TestFailureException(ss.str());                            \
        }                                                                                          \
    } while (0)

#define EXPECT_NE(val1, val2)                                                                      \
    do {                                                                                           \
        if (!((val1) != (val2))) {                                                                 \
            std::ostringstream ss;                                                                 \
            ss << __FILE__ << ":" << __LINE__ << " Check failed: EXPECT_NE(" #val1 ", " #val2 ")"; \
            std::cerr << ss.str() << std::endl;                                                    \
            throw ::stripekit::testing::TestFailureException(ss.str());                            \
        }                                                                                          \
    } while (0)

#define EXPECT_GE(val1, val2)                                                                      \
    do {                                                                                           \
        if (!((val1) >= (val2))) {                                                                 \
            std::ostringstream ss;                                                                 \
            ss << __FILE__ << ":" << __LINE__                                                      \
               << " Check failed: EXPECT_GE(" #val1 ", " #val2 ") ["                               \
               << ::stripekit::testing::format_val(val1) << " vs "                                 \
               << ::stripekit::testing::format_val(val2) << "]";                                   \
            std::cerr << ss.str() << std::endl;                                                    \
            throw ::stripekit::testing::TestFailureException(ss.str());                            \
        }                                                                                          \
    } while (0)

#define EXPECT_STREQ(s1, s2)                                                                       \
    do {                                                                                           \
        if (std::string(s1) != std::string(s2)) {                                                  \
            std::ostringstream ss;                                                                 \
            ss << __FILE__ << ":" << __LINE__ << " Check failed: EXPECT_STREQ(\"" << (s1)          \
               << "\", \"" << (s2) << "\")";                                                       \
            std::cerr << ss.str() << std::endl;                                                    \
            throw ::stripekit::testing::TestFailureException(ss.str());                            \
        }                                                                                          \
    } while (0)

#define EXPECT_THROW(statement, expected_exception)                                                \
    do {                                                                                           \
        bool caught = false;                                                                       \
        try {                                                                                      \
            statement;                                                                             \
        } catch (const expected_exception&) {                                                      \
            caught = true;                                                                         \
        } catch (...) {                                                                            \
            std::ostringstream ss;                                                                 \
            ss << __FILE__ << ":" << __LINE__                                                      \
               << " EXPECT_THROW(" #statement ", " #expected_exception                             \
                  ") caught different exception";                                                  \
            std::cerr << ss.str() << std::endl;                                                    \
            throw ::stripekit::testing::TestFailureException(ss.str());                            \
        }                                                                                          \
        if (!caught) {                                                                             \
            std::ostringstream ss;                                                                 \
            ss << __FILE__ << ":" << __LINE__                                                      \
               << " Expected exception " #expected_exception " not thrown by " #statement;         \
            std::cerr << ss.str() << std::endl;                                                    \
            throw ::stripekit::testing::TestFailureException(ss.str());                            \
        }                                                                                          \
    } while (0)

#define EXPECT_NO_THROW(statement)                                                                 \
    do {                                                                                           \
        try {                                                                                      \
            statement;                                                                             \
        } catch (const std::exception& ex) {                                                       \
            std::ostringstream ss;                                                                 \
            ss << __FILE__ << ":" << __LINE__                                                      \
               << " Statement " #statement " threw exception: " << ex.what();                      \
            std::cerr << ss.str() << std::endl;                                                    \
            throw ::stripekit::testing::TestFailureException(ss.str());                            \
        } catch (...) {                                                                            \
            std::ostringstream ss;                                                                 \
            ss << __FILE__ << ":" << __LINE__                                                      \
               << " Statement " #statement " threw unknown exception";                             \
            std::cerr << ss.str() << std::endl;                                                    \
            throw ::stripekit::testing::TestFailureException(ss.str());                            \
        }                                                                                          \
    } while (0)

#define ASSERT_TRUE(expr) EXPECT_TRUE(expr)
#define ASSERT_FALSE(expr) EXPECT_FALSE(expr)
#define ASSERT_EQ(val1, val2) EXPECT_EQ(val1, val2)
#define ASSERT_NE(val1, val2) EXPECT_NE(val1, val2)

class FailStream {
public:
    FailStream(const char* file, int line) {
        ss << file << ":" << line << " Test forced failure: ";
    }

    template <typename T>
    FailStream& operator<<(const T& val) {
        ss << val;
        return *this;
    }

    [[noreturn]] ~FailStream() noexcept(false) {
        std::cerr << ss.str() << std::endl;
        throw ::stripekit::testing::TestFailureException(ss.str());
    }

private:
    std::ostringstream ss;
};

#define FAIL() ::stripekit::testing::FailStream(__FILE__, __LINE__)

struct TestFixtureBase {
    virtual ~TestFixtureBase() = default;
    virtual void SetUp() {}
    virtual void TearDown() {}
};

} // namespace stripekit::testing

namespace testing {
using Test = ::stripekit::testing::TestFixtureBase;
} // namespace testing
