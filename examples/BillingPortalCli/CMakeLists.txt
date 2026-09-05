add_executable(stripekit_billing_portal_cli
    BillingPortalCli.cpp
)

target_link_libraries(stripekit_billing_portal_cli PRIVATE StripeKit::drogon)

set_target_properties(stripekit_billing_portal_cli PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)