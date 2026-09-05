if(NOT TARGET StripeKit::drogon)
    message(FATAL_ERROR "STRIPEKIT_BUILD_EXAMPLES requires STRIPEKIT_BUILD_DROGON_ADAPTER=ON")
endif()

add_subdirectory(DrogonCheckoutDemo)
add_subdirectory(WebhookSignature)
add_subdirectory(CheckoutSessionCli)
add_subdirectory(BillingPortalCli)