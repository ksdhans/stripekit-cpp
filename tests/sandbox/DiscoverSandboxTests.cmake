# Register each sandbox fixture as a separate CTest test.
if(NOT EXISTS "${sandbox_executable}")
    add_test(stripekit.sandbox.NOT_BUILT "${sandbox_executable}" --list)
    set_tests_properties(stripekit.sandbox.NOT_BUILT PROPERTIES LABELS sandbox)
    return()
endif()

execute_process(
    COMMAND "${sandbox_executable}" --list
    RESULT_VARIABLE list_result
    OUTPUT_VARIABLE test_list
    ERROR_VARIABLE list_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT list_result STREQUAL "0")
    message(FATAL_ERROR "Could not list sandbox tests: ${list_result} ${list_error}")
endif()
if(test_list STREQUAL "")
    message(FATAL_ERROR "The sandbox executable listed no tests")
endif()

string(REPLACE "\r\n" "\n" test_list "${test_list}")
string(REPLACE "\n" ";" test_list "${test_list}")
set(suites)
foreach(test IN LISTS test_list)
    if(NOT test MATCHES "^([A-Za-z_][A-Za-z0-9_]*)\\.[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "Invalid sandbox test name: ${test}")
    endif()
    list(APPEND suites "${CMAKE_MATCH_1}")
endforeach()
list(REMOVE_DUPLICATES suites)

foreach(suite IN LISTS suites)
    set(name "stripekit.sandbox.${suite}")
    add_test("${name}" "${sandbox_executable}" --suite "${suite}")
    set_tests_properties("${name}" PROPERTIES
        LABELS sandbox
        ENVIRONMENT "STRIPEKIT_TEST_PROGRESS_LOG=${sandbox_log_directory}/stripekit-sandbox-${suite}-progress.log"
        RUN_SERIAL TRUE
        TIMEOUT 900
    )
endforeach()
