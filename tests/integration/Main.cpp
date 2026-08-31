/** Main.cpp
 * 
 * Integration test executable entry point for CTest.
 * 
 * @author Hans de Ruiter
 * 
 * @license See LICENSE.md for details.
 */

#include "IntegrationTestServer.h"
#include "TestFramework.h"

int main(int argc, char* argv[]) {
    if (argc > 1 && (std::string(argv[1]) == "--list" || std::string(argv[1]) == "-l")) {
        stripekit::testing::TestRegistry::instance().list_tests();
        return 0;
    }

    std::string filter;
    if (argc > 1) {
        filter = argv[1];
    }

    stripekit::testing::DrogonIntegrationEnvironment::get().start();
    const int result = stripekit::testing::TestRegistry::instance().run_all(filter);
    stripekit::testing::DrogonIntegrationEnvironment::get().stop();

    return result;
}
