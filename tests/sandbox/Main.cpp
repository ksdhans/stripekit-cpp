/** Main.cpp
 *
 * Sandbox end-to-end test executable entry point for CTest.
 *
 * @author Hans de Ruiter
 *
 * @license See LICENSE.md for details.
 */

#include "SandboxEnvironment.h"
#include "TestFramework.h"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc > 1 && (std::string(argv[1]) == "--list" || std::string(argv[1]) == "-l")) {
        stripekit::testing::TestRegistry::instance().list_tests();
        return 0;
    }

    std::string filter;
    if (argc > 1) {
        filter = argv[1];
    }

    auto& environment = stripekit::testing::SandboxEnvironment::get();

    try {
        if (!environment.start()) {
            const std::string skip_message = "[  SKIPPED ] Stripe sandbox E2E tests: " +
                                             environment.unavailable_reason();
            std::cout << skip_message << std::endl;
            stripekit::testing::append_test_progress(skip_message);
            std::cout << "See tests/sandbox/README.md for setup instructions." << std::endl;

            const auto& config = environment.config();
            return (config.required || config.is_unsafe()) ? 1 : 0;
        }
    } catch (const std::exception& error) {
        const std::string failure_message = "Could not start the Stripe sandbox environment: " +
                                            std::string(error.what());
        std::cerr << failure_message << std::endl;
        stripekit::testing::append_test_progress(failure_message);
        return 1;
    }

    const int result = stripekit::testing::TestRegistry::instance().run_all(filter);
    environment.stop();

    return result;
}
