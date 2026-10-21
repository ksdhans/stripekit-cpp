add_executable(stripekit_invoice_status_cli
    InvoiceStatusCli.cpp
)

target_link_libraries(stripekit_invoice_status_cli PRIVATE StripeKit::drogon)

set_target_properties(stripekit_invoice_status_cli PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
