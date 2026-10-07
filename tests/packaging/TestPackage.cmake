# Installs into a private prefix and checks a separate downstream project.
set(test_dir "${STRIPEKIT_BUILD_DIR}/package-test")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${STRIPEKIT_BUILD_DIR}"
        --prefix "${test_dir}/install" --config "${STRIPEKIT_TEST_CONFIG}"
    COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${STRIPEKIT_CONSUMER_SOURCE}" -B "${test_dir}/consumer"
        -G "${STRIPEKIT_TEST_GENERATOR}"
        "-DCMAKE_CXX_COMPILER=${STRIPEKIT_TEST_COMPILER}"
        "-DCMAKE_PREFIX_PATH=${test_dir}/install"
        "-DCMAKE_BUILD_TYPE=${STRIPEKIT_TEST_CONFIG}"
    COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${test_dir}/consumer"
        --config "${STRIPEKIT_TEST_CONFIG}"
    COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${test_dir}/consumer"
        -C "${STRIPEKIT_TEST_CONFIG}" --output-on-failure
    COMMAND_ERROR_IS_FATAL ANY
)
