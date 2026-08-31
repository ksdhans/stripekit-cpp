/** Main.cpp
 * 
 * Unit test executable entry point for CTest.
 * 
 * @author Hans de Ruiter
 * 
 * @license See LICENSE.md for details.
 */

#include "TestFramework.h"

int main(int argc, char* argv[]) {
    std::string filter;
    if (argc > 1) {
        filter = argv[1];
    }
    return stripekit::testing::TestRegistry::instance().run_all(filter);
}
