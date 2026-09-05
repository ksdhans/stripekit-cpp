add_executable(stripekit_webhook_signature_example
    WebhookSignature.cpp
)

target_link_libraries(stripekit_webhook_signature_example PRIVATE StripeKit::drogon)

set_target_properties(stripekit_webhook_signature_example PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)