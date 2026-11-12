# Optional, source-tree test support; not part of StripeKit's installed API.
add_library(stripekit_sandbox_support STATIC TaxSandbox.cpp)
add_library(StripeKit::sandbox_support ALIAS stripekit_sandbox_support)
target_compile_features(stripekit_sandbox_support PUBLIC cxx_std_20)
target_include_directories(stripekit_sandbox_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(stripekit_sandbox_support PUBLIC StripeKit::stripekit)
