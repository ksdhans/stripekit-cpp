# Build script for StripeKit tests.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(stripekit_unit_tests
    unit/Main.cpp
    unit/TestBillingPortalAndInvoices.cpp
    unit/TestCatalog.cpp
    unit/TestLogging.cpp
    unit/TestModelsAndParser.cpp
    unit/TestPayments.cpp
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
            sandbox/TestDisputeAndRefund.cpp
            sandbox/TestPayments.cpp
            sandbox/TestSubscriptionLifecycle.cpp
            sandbox/TestSubscriptionTrials.cpp
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
        )

        add_test(NAME stripekit.sandbox COMMAND stripekit_sandbox_tests)

        # Sandbox tests share one Stripe sandbox and wait on live event delivery,
        # so they must not compete with other tests for it.
        set_tests_properties(stripekit.sandbox PROPERTIES
            LABELS sandbox
            ENVIRONMENT "STRIPEKIT_TEST_PROGRESS_LOG=${CMAKE_BINARY_DIR}/stripekit-sandbox-progress.log"
            RUN_SERIAL TRUE
            TIMEOUT 900
        )
    endif()
endif()
