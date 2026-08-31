# Build script for StripeKit tests.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    FIND_PACKAGE_ARGS 1.14.0 CONFIG QUIET
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(stripekit_unit_tests
    unit/TestLogging.cpp
    unit/TestModelsAndParser.cpp
    unit/TestSqliteWebhookDedupStore.cpp
    unit/TestStripeApiClient.cpp
    unit/TestVersionAndErrors.cpp
    unit/TestWebhookDedupStore.cpp
    unit/TestWebhookProcessor.cpp
    unit/TestWebhookVerifier.cpp
    unit/TestWebhooks.cpp
)

set_target_properties(stripekit_unit_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(stripekit_unit_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/unit
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/include
)

target_link_libraries(stripekit_unit_tests
    PRIVATE
        StripeKit::stripekit
        GTest::gtest_main
)

if(stripekit_libsodium_target)
    target_link_libraries(stripekit_unit_tests PRIVATE ${stripekit_libsodium_target})
    target_compile_definitions(stripekit_unit_tests PRIVATE STRIPEKIT_WITH_LIBSODIUM=1)
endif()

if(TARGET SQLite::SQLite3)
    target_link_libraries(stripekit_unit_tests PRIVATE SQLite::SQLite3)
    target_compile_definitions(stripekit_unit_tests PRIVATE STRIPEKIT_WITH_SQLITE3=1)
endif()

include(GoogleTest)
gtest_discover_tests(stripekit_unit_tests)

if("drogon" IN_LIST STRIPEKIT_ENABLED_ADAPTERS)
	add_test(NAME stripekit.drogon.smoke COMMAND ${CMAKE_COMMAND} -E true)
endif()
