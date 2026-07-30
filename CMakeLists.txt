cmake_minimum_required(VERSION 3.24)

project(StripeKit
    VERSION 0.1.0
    DESCRIPTION "Stripe integration toolkit for C++"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(STRIPEKIT_BUILD_TESTS "Build StripeKit tests" ON)

add_library(stripekit
    src/core/stripekit.cpp
)
add_library(StripeKit::stripekit ALIAS stripekit)

target_compile_features(stripekit PUBLIC cxx_std_20)

target_include_directories(stripekit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

include(cmake/generate_version_header.cmake)
add_dependencies(stripekit generate_${PROJECT_NAME}_version_header)

set(STRIPEKIT_CMAKE_CONFIG_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/StripeKit")

configure_package_config_file(
    cmake/StripeKitConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/StripeKitConfig.cmake
    INSTALL_DESTINATION ${STRIPEKIT_CMAKE_CONFIG_DIR}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/StripeKitConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(TARGETS stripekit
    EXPORT StripeKitTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/include/stripekit_rev.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT StripeKitTargets
    FILE StripeKitTargets.cmake
    NAMESPACE StripeKit::
    DESTINATION ${STRIPEKIT_CMAKE_CONFIG_DIR}
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/StripeKitConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/StripeKitConfigVersion.cmake
    DESTINATION ${STRIPEKIT_CMAKE_CONFIG_DIR}
)

if(STRIPEKIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
