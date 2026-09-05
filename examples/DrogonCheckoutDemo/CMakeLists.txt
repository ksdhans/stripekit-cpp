add_executable(stripekit_drogon_checkout_demo
    DrogonCheckoutDemo.cpp
)

target_link_libraries(stripekit_drogon_checkout_demo PRIVATE StripeKit::drogon)
target_include_directories(stripekit_drogon_checkout_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT COMMAND drogon_create_views)
    if(DEFINED drogon_SOURCE_DIR AND EXISTS "${drogon_SOURCE_DIR}/cmake/DrogonUtilities.cmake")
        include("${drogon_SOURCE_DIR}/cmake/DrogonUtilities.cmake")
    elseif(DEFINED Drogon_SOURCE_DIR AND EXISTS "${Drogon_SOURCE_DIR}/cmake/DrogonUtilities.cmake")
        include("${Drogon_SOURCE_DIR}/cmake/DrogonUtilities.cmake")
    endif()
endif()

if(NOT COMMAND drogon_create_views)
    message(FATAL_ERROR "Drogon CSP templates require drogon_create_views from DrogonUtilities.cmake")
endif()

drogon_create_views(stripekit_drogon_checkout_demo
    ${CMAKE_CURRENT_SOURCE_DIR}/Views
    ${CMAKE_CURRENT_BINARY_DIR}/Views
)

if(TARGET drogon_ctl)
    add_dependencies(stripekit_drogon_checkout_demo drogon_ctl)
endif()

set_target_properties(stripekit_drogon_checkout_demo PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)