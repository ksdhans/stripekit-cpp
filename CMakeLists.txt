cmake_minimum_required(VERSION 3.24)

project(StripeKit
    VERSION 0.1.0
    DESCRIPTION "Stripe integration toolkit for C++"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FetchContent)

option(STRIPEKIT_BUILD_TESTS "Build StripeKit tests" ON)
option(STRIPEKIT_ENABLE_SQLITE "Enable optional SQLite webhook de-dup store adapter" ON)

option(USE_EXTERNAL_LIBSODIUM "Reuse libsodium provided by the parent project or system installation" OFF)
option(USE_EXTERNAL_DROGON "Reuse Drogon provided by the parent project or system installation" OFF)

add_library(stripekit
    src/core/StripeKit.cpp
    src/core/Errors.cpp
    src/core/Logging.cpp
    src/core/WebhookDedupStore.cpp
    src/core/WebhookVerifier.cpp
    src/core/SqliteWebhookDedupStore.cpp
)
add_library(StripeKit::stripekit ALIAS stripekit)

target_compile_features(stripekit PUBLIC cxx_std_20)

target_include_directories(stripekit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/include
)

if(TARGET sodium)
    set(stripekit_libsodium_target sodium)
elseif(TARGET unofficial-sodium::sodium)
    set(stripekit_libsodium_target unofficial-sodium::sodium)
endif()

if(USE_EXTERNAL_LIBSODIUM)
    find_package(sodium CONFIG QUIET)
    find_package(unofficial-sodium CONFIG QUIET)

    if(TARGET sodium)
        set(stripekit_libsodium_target sodium)
    elseif(TARGET unofficial-sodium::sodium)
        set(stripekit_libsodium_target unofficial-sodium::sodium)
    else()
        message(FATAL_ERROR "USE_EXTERNAL_LIBSODIUM is ON, but StripeKit could not find libsodium. Add libsodium before StripeKit, or point CMake at the install prefix with CMAKE_PREFIX_PATH, libsodium_ROOT, or libsodium_DIR.")
    endif()
else()
    FetchContent_Declare(
        stripekit_libsodium
        GIT_REPOSITORY https://github.com/jedisct1/libsodium.git
        GIT_TAG 1.0.20-RELEASE
    )
    set(SODIUM_DISABLE_TESTS ON CACHE BOOL "" FORCE)
    set(SODIUM_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(stripekit_libsodium)

    if(TARGET sodium)
        set(stripekit_libsodium_target sodium)
    elseif(TARGET libsodium)
        set(stripekit_libsodium_target libsodium)
    endif()
endif()

if(stripekit_libsodium_target)
    target_link_libraries(stripekit PRIVATE ${stripekit_libsodium_target})
    target_compile_definitions(stripekit PRIVATE STRIPEKIT_WITH_LIBSODIUM=1)
else()
    message(STATUS "StripeKit: libsodium not found; webhook signature verification support is disabled")
endif()

if(USE_EXTERNAL_DROGON)
    set(stripekit_drogon_target "")

    if(TARGET Drogon::Drogon)
        set(stripekit_drogon_target Drogon::Drogon)
    elseif(TARGET drogon)
        set(stripekit_drogon_target drogon)
    endif()

    if(NOT stripekit_drogon_target)
        find_package(Drogon CONFIG QUIET)

        if(TARGET Drogon::Drogon)
            set(stripekit_drogon_target Drogon::Drogon)
        elseif(TARGET drogon)
            set(stripekit_drogon_target drogon)
        else()
            message(FATAL_ERROR "USE_EXTERNAL_DROGON is ON, but StripeKit could not find Drogon. Add Drogon before StripeKit, or point CMake at the install prefix with CMAKE_PREFIX_PATH, Drogon_ROOT, or Drogon_DIR.")
        endif()
    endif()

    if(stripekit_drogon_target)
        add_library(StripeKit::drogon_dependency INTERFACE IMPORTED)
        set_property(TARGET StripeKit::drogon_dependency PROPERTY
            INTERFACE_LINK_LIBRARIES ${stripekit_drogon_target}
        )
    endif()
endif()

if(STRIPEKIT_ENABLE_SQLITE)
    find_package(SQLite3 QUIET)
    if(TARGET SQLite::SQLite3)
        target_link_libraries(stripekit PRIVATE SQLite::SQLite3)
        target_compile_definitions(stripekit PRIVATE STRIPEKIT_WITH_SQLITE3=1)
    endif()
endif()

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
