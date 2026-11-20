# Build script for StripeKit tests.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(support)

add_executable(stripekit_unit_tests
    unit/Main.cpp
    unit/TestBillingPortalAndInvoices.cpp
    unit/TestCatalog.cpp
    unit/TestCollectionAndInvoices.cpp
    unit/TestSubscriptionSchedules.cpp
    unit/TestTaxSandbox.cpp
    unit/TestEventHistory.cpp
    unit/TestLogging.cpp
    unit/TestModelsAndParser.cpp
    unit/TestPayments.cpp
    unit/TestRefunds.cpp
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
        StripeKit::sandbox_support
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

if(UNIX AND TARGET stripekit_drogon_checkout_demo)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME stripekit.checkout-demo
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/integration/TestCheckoutDemo.py
                $<TARGET_FILE:stripekit_drogon_checkout_demo>
        )
        set_tests_properties(stripekit.checkout-demo PROPERTIES TIMEOUT 30)
        if(TARGET stripekit_product_catalog_cli)
            add_test(NAME stripekit.product-catalog-cli
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/integration/TestProductCatalogCli.py
                    $<TARGET_FILE:stripekit_product_catalog_cli>
            )
            set_tests_properties(stripekit.product-catalog-cli PROPERTIES TIMEOUT 30)
        endif()
    else()
        message(STATUS "StripeKit: Python3 not found; checkout demo regression test is disabled")
    endif()
endif()

if(STRIPEKIT_STANDALONE_PROJECT)
    add_test(NAME stripekit.dependency-reuse
        COMMAND ${CMAKE_COMMAND}
            -S ${CMAKE_CURRENT_SOURCE_DIR}/packaging/DependencyReuse
            -B ${CMAKE_BINARY_DIR}/dependency-reuse-test
            "-DSTRIPEKIT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    )
    set_tests_properties(stripekit.dependency-reuse PROPERTIES LABELS packaging TIMEOUT 120)
    add_test(NAME stripekit.packaging
        COMMAND ${CMAKE_COMMAND}
            -DSTRIPEKIT_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DSTRIPEKIT_CONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/packaging
            -DSTRIPEKIT_TEST_CONFIG=$<CONFIG>
            -DSTRIPEKIT_TEST_GENERATOR=${CMAKE_GENERATOR}
            -DSTRIPEKIT_TEST_COMPILER=${CMAKE_CXX_COMPILER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/packaging/TestPackage.cmake
    )
    set_tests_properties(stripekit.packaging PROPERTIES LABELS packaging TIMEOUT 120)
endif()

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
    add_test(NAME stripekit.webhook-coroutine
        COMMAND stripekit_integration_tests CoroutineHandlerCanSuspendOnDrogonEventLoop)
    set_tests_properties(stripekit.webhook-coroutine PROPERTIES TIMEOUT 15)

    if(STRIPEKIT_BUILD_SANDBOX_TESTS)
        add_executable(stripekit_sandbox_tests
            sandbox/Main.cpp
            sandbox/SandboxEnvironment.cpp
            sandbox/TestBillingPortalAndInvoices.cpp
            sandbox/TestCatalog.cpp
            sandbox/TestCollectionAndInvoices.cpp
            sandbox/TestSubscriptionSchedules.cpp
            sandbox/TestDisputeAndRefund.cpp
            sandbox/TestPayments.cpp
            sandbox/TestSubscriptionLifecycle.cpp
            sandbox/TestSubscriptionTrials.cpp
            sandbox/TestTax.cpp
            sandbox/TestWebhookDelivery.cpp
        )

        set_target_properties(stripekit_sandbox_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
        )

        target_include_directories(stripekit_sandbox_tests
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}
                ${CMAKE_CURRENT_SOURCE_DIR}/sandbox
                ${CMAKE_CURRENT_SOURCE_DIR}/../include
                ${CMAKE_CURRENT_SOURCE_DIR}/../src/core
                ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/include
                ${CMAKE_CURRENT_SOURCE_DIR}/../src/adapters/drogon/include
        )

        target_link_libraries(stripekit_sandbox_tests
            PRIVATE
                StripeKit::stripekit
                StripeKit::drogon
                StripeKit::sandbox_support
        )

        file(GENERATE
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/DiscoverSandboxTests-$<CONFIG>.cmake"
            CONTENT "set(sandbox_executable [==[$<TARGET_FILE:stripekit_sandbox_tests>]==])
set(sandbox_log_directory [==[${CMAKE_BINARY_DIR}]==])
include([==[${CMAKE_CURRENT_SOURCE_DIR}/sandbox/DiscoverSandboxTests.cmake]==])
"
        )
        get_property(sandbox_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
        file(CONFIGURE
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/IncludeSandboxTests.cmake"
            CONTENT [=[
if(@sandbox_multi_config@)
    string(TOLOWER "${CTEST_CONFIGURATION_TYPE}" requested_configuration)
    foreach(configuration IN ITEMS @CMAKE_CONFIGURATION_TYPES@)
        string(TOLOWER "${configuration}" lowercase_configuration)
        if(lowercase_configuration STREQUAL requested_configuration)
            set(sandbox_configuration "${configuration}")
            break()
        endif()
    endforeach()
    if(NOT DEFINED sandbox_configuration)
        add_test(stripekit.sandbox.CONFIGURATION_REQUIRED "@CMAKE_COMMAND@" -E false)
        set_tests_properties(stripekit.sandbox.CONFIGURATION_REQUIRED PROPERTIES LABELS sandbox)
        return()
    endif()
else()
    set(sandbox_configuration "@CMAKE_BUILD_TYPE@")
endif()
include("@CMAKE_CURRENT_BINARY_DIR@/DiscoverSandboxTests-${sandbox_configuration}.cmake")
]=]
            @ONLY
        )
        set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
            "${CMAKE_CURRENT_BINARY_DIR}/IncludeSandboxTests.cmake")
        add_test(NAME stripekit.sandbox-discovery
            COMMAND ${CMAKE_COMMAND}
                "-DCTEST_EXECUTABLE=${CMAKE_CTEST_COMMAND}"
                "-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
                "-DSANDBOX_EXECUTABLE=$<TARGET_FILE:stripekit_sandbox_tests>"
                "-DTEST_CONFIGURATION=$<CONFIG>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/sandbox/TestSandboxDiscovery.cmake"
        )
    endif()
endif()
