cmake_minimum_required(VERSION 3.24)

project(StripeKit
    VERSION 0.1.0
    DESCRIPTION "Stripe integration toolkit for C++"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(ExternalProject)
include(FetchContent)

option(STRIPEKIT_BUILD_TESTS "Build StripeKit tests" ON)
option(STRIPEKIT_BUILD_SANDBOX_TESTS "Build the Stripe sandbox end-to-end tests" OFF)
option(STRIPEKIT_ENABLE_SQLITE "Enable optional SQLite webhook de-dup store adapter" ON)
option(STRIPEKIT_BUILD_DROGON_ADAPTER "Build the Drogon runtime adapter" ON)

option(USE_EXTERNAL_LIBSODIUM "Reuse libsodium provided by the parent project or system installation" OFF)
option(USE_EXTERNAL_DROGON "Reuse Drogon provided by the parent project or system installation" OFF)

# Drogon forces BUILD_SHARED_LIBS ON for the whole build, so StripeKit's own
# libraries pin their type explicitly instead of inheriting that setting.
option(STRIPEKIT_LIB_SHARED "Build StripeKit's libraries as shared libraries" OFF)
if(STRIPEKIT_LIB_SHARED)
    set(STRIPEKIT_LIB_TYPE SHARED)
else()
    set(STRIPEKIT_LIB_TYPE STATIC)
endif()

add_library(stripekit ${STRIPEKIT_LIB_TYPE}
    src/core/StripeKit.cpp
    src/core/Errors.cpp
    src/core/Logging.cpp
    src/core/WebhookDedupStore.cpp
    src/core/WebhookProcessor.cpp
    src/core/WebhookVerifier.cpp
    src/core/Webhooks.cpp
    src/core/SqliteWebhookDedupStore.cpp
    src/core/StripeApiClient.cpp
)
add_library(StripeKit::stripekit ALIAS stripekit)

target_compile_features(stripekit PUBLIC cxx_std_20)

target_include_directories(stripekit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
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
    set(STRIPEKIT_LIBSODIUM_VERSION 1.0.20)
    set(stripekit_libsodium_install_dir ${CMAKE_CURRENT_BINARY_DIR}/_deps/libsodium-install)
    file(MAKE_DIRECTORY ${stripekit_libsodium_install_dir}/include)

    ExternalProject_Add(stripekit_libsodium_external
        URL https://download.libsodium.org/libsodium/releases/libsodium-${STRIPEKIT_LIBSODIUM_VERSION}.tar.gz
        CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --disable-shared --enable-static --disable-dependency-tracking
        BUILD_COMMAND make
        INSTALL_COMMAND make install
        INSTALL_DIR ${stripekit_libsodium_install_dir}
        BUILD_BYPRODUCTS ${stripekit_libsodium_install_dir}/lib/libsodium.a
    )

    add_library(stripekit_libsodium STATIC IMPORTED GLOBAL)
    add_dependencies(stripekit_libsodium stripekit_libsodium_external)
    set_target_properties(stripekit_libsodium PROPERTIES
        IMPORTED_LOCATION ${stripekit_libsodium_install_dir}/lib/libsodium.a
        INTERFACE_INCLUDE_DIRECTORIES ${stripekit_libsodium_install_dir}/include
    )
    set(stripekit_libsodium_target stripekit_libsodium)
endif()

if(stripekit_libsodium_target)
    target_link_libraries(stripekit PRIVATE ${stripekit_libsodium_target})
    target_compile_definitions(stripekit PRIVATE STRIPEKIT_WITH_LIBSODIUM=1)
else()
    message(STATUS "StripeKit: libsodium not found; webhook signature verification support is disabled")
endif()

set(stripekit_jsoncpp_target "")
if(TARGET JsonCpp::JsonCpp)
    set(stripekit_jsoncpp_target JsonCpp::JsonCpp)
elseif(TARGET jsoncpp_lib)
    set(stripekit_jsoncpp_target jsoncpp_lib)
elseif(TARGET jsoncpp_static)
    set(stripekit_jsoncpp_target jsoncpp_static)
endif()

if(NOT stripekit_jsoncpp_target)
    set(STRIPEKIT_JSONCPP_VERSION 1.9.5)
    # No FIND_PACKAGE_ARGS here: CMake's FetchContent find_package redirect is
    # global, and would hijack Drogon's own find_package(Jsoncpp) lookup,
    # leaving it without the "Jsoncpp_lib" target it expects.
    FetchContent_Declare(
        jsoncpp
        URL https://github.com/open-source-parsers/jsoncpp/archive/refs/tags/${STRIPEKIT_JSONCPP_VERSION}.tar.gz
    )
    set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(jsoncpp)

    if(TARGET JsonCpp::JsonCpp)
        set(stripekit_jsoncpp_target JsonCpp::JsonCpp)
    elseif(TARGET jsoncpp_lib)
        set(stripekit_jsoncpp_target jsoncpp_lib)
    elseif(TARGET jsoncpp_static)
        set(stripekit_jsoncpp_target jsoncpp_static)
    endif()
endif()

if(NOT stripekit_jsoncpp_target)
    message(FATAL_ERROR "StripeKit could not find or fetch JsonCpp.")
endif()

target_link_libraries(stripekit PUBLIC ${stripekit_jsoncpp_target})

if(STRIPEKIT_BUILD_DROGON_ADAPTER)
    set(STRIPEKIT_DROGON_VERSION 1.9.10)
    set(stripekit_drogon_target "")

    if(USE_EXTERNAL_DROGON)
        if(TARGET Drogon::Drogon)
            set(stripekit_drogon_target Drogon::Drogon)
        elseif(TARGET drogon)
            set(stripekit_drogon_target drogon)
        endif()

        if(NOT stripekit_drogon_target)
            FetchContent_Declare(
                Drogon
                GIT_REPOSITORY https://github.com/drogonframework/drogon.git
                GIT_TAG v${STRIPEKIT_DROGON_VERSION}
                GIT_SUBMODULES_RECURSE TRUE
                FIND_PACKAGE_ARGS ${STRIPEKIT_DROGON_VERSION} CONFIG REQUIRED
            )
            FetchContent_MakeAvailable(Drogon)

            if(TARGET Drogon::Drogon)
                set(stripekit_drogon_target Drogon::Drogon)
            elseif(TARGET drogon)
                set(stripekit_drogon_target drogon)
            endif()
        endif()
    else()
        FetchContent_Declare(
            Drogon
            GIT_REPOSITORY https://github.com/drogonframework/drogon.git
            GIT_TAG v${STRIPEKIT_DROGON_VERSION}
            GIT_SUBMODULES_RECURSE TRUE
            FIND_PACKAGE_ARGS ${STRIPEKIT_DROGON_VERSION} CONFIG QUIET
        )
        set(BUILD_CTL OFF CACHE BOOL "" FORCE)
        set(BUILD_ORM OFF CACHE BOOL "" FORCE)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(Drogon)

        if(TARGET Drogon::Drogon)
            set(stripekit_drogon_target Drogon::Drogon)
        elseif(TARGET drogon)
            set(stripekit_drogon_target drogon)
        endif()
    endif()

    if(NOT stripekit_drogon_target)
        message(FATAL_ERROR "StripeKit could not find or fetch Drogon.")
    endif()

    add_library(stripekit_drogon ${STRIPEKIT_LIB_TYPE}
        src/adapters/drogon/DrogonHTTPClient.cpp
        src/adapters/drogon/DrogonLogger.cpp
        src/adapters/drogon/DrogonWebhookEndpoint.cpp
    )
    add_library(StripeKit::drogon ALIAS stripekit_drogon)
    set_target_properties(stripekit_drogon PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
    target_link_libraries(stripekit_drogon PUBLIC StripeKit::stripekit ${stripekit_drogon_target})
    target_include_directories(stripekit_drogon
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/drogon/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
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

set(STRIPEKIT_CONFIG_FIND_DROGON "")
if(TARGET stripekit_drogon)
    set(STRIPEKIT_CONFIG_FIND_DROGON "find_dependency(Drogon CONFIG)")
endif()

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

if(TARGET stripekit_drogon)
    install(TARGETS stripekit_drogon
        EXPORT StripeKitTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(DIRECTORY src/adapters/drogon/include/StripeKit/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/StripeKit
    )
endif()

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY src/core/include/StripeKit/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/StripeKit
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
    set(STRIPEKIT_ENABLED_ADAPTERS "")
    if(TARGET stripekit_drogon)
        list(APPEND STRIPEKIT_ENABLED_ADAPTERS drogon)
    endif()

    if(NOT STRIPEKIT_ENABLED_ADAPTERS)
        message(FATAL_ERROR "STRIPEKIT_BUILD_TESTS is ON, but no StripeKit adapter is enabled. Enable at least one adapter, for example STRIPEKIT_BUILD_DROGON_ADAPTER.")
    endif()

    enable_testing()
    add_subdirectory(tests)
endif()
