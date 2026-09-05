add_executable(stripekit_checkout_session_cli
    CheckoutSessionCli.cpp
)

target_link_libraries(stripekit_checkout_session_cli PRIVATE StripeKit::drogon)

set_target_properties(stripekit_checkout_session_cli PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)