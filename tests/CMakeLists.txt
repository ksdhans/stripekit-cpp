# Build script for StripeKit tests.

if("drogon" IN_LIST STRIPEKIT_ENABLED_ADAPTERS)
	add_test(NAME stripekit.drogon.smoke COMMAND ${CMAKE_COMMAND} -E true)
endif()
