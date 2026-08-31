# Build script for StripeKit tests.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(stripekit_unit_tests
    unit/Main.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/unit
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/include
)

target_link_libraries(stripekit_unit_tests
    PRIVATE
        StripeKit::stripekit
)

if(stripekit_libsodium_target)
    target_link_libraries(stripekit_unit_tests PRIVATE ${stripekit_libsodium_target})
    target_compile_definitions(stripekit_unit_tests PRIVATE STRIPEKIT_WITH_LIBSODIUM=1)
endif()

if(TARGET SQLite::SQLite3)
    target_link_libraries(stripekit_unit_tests PRIVATE SQLite::SQLite3)
    target_compile_definitions(stripekit_unit_tests PRIVATE STRIPEKIT_WITH_SQLITE3=1)
endif()

add_test(NAME stripekit.unit COMMAND stripekit_unit_tests)

if("drogon" IN_LIST STRIPEKIT_ENABLED_ADAPTERS)
    add_executable(stripekit_integration_tests
        integration/Main.cpp
        integration/TestDrogonHTTPClient.cpp
        integration/TestDrogonWebhookEndpoint.cpp
        integration/TestDrogonLogger.cpp
    )

    set_target_properties(stripekit_integration_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )

    target_include_directories(stripekit_integration_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/integration
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/adapters/drogon/include
    )

    target_link_libraries(stripekit_integration_tests
        PRIVATE
            StripeKit::stripekit
            StripeKit::drogon
    )

    if(stripekit_libsodium_target)
        target_link_libraries(stripekit_integration_tests PRIVATE ${stripekit_libsodium_target})
        target_compile_definitions(stripekit_integration_tests PRIVATE STRIPEKIT_WITH_LIBSODIUM=1)
    endif()

    if(TARGET SQLite::SQLite3)
        target_link_libraries(stripekit_integration_tests PRIVATE SQLite::SQLite3)
        target_compile_definitions(stripekit_integration_tests PRIVATE STRIPEKIT_WITH_SQLITE3=1)
    endif()

    add_test(NAME stripekit.integration COMMAND stripekit_integration_tests)
endif()
