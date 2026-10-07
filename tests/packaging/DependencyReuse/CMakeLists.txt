cmake_minimum_required(VERSION 3.24)
project(StripeKitDependencyReuse LANGUAGES CXX)

# Imported parent targets must be reused even when external-only options are off.
add_library(sodium INTERFACE IMPORTED GLOBAL)
add_library(Drogon::Drogon INTERFACE IMPORTED GLOBAL)
set(USE_EXTERNAL_LIBSODIUM OFF CACHE BOOL "" FORCE)
set(USE_EXTERNAL_DROGON OFF CACHE BOOL "" FORCE)
set(STRIPEKIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(STRIPEKIT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(STRIPEKIT_BUILD_DOCS OFF CACHE BOOL "" FORCE)
add_subdirectory("${STRIPEKIT_SOURCE_DIR}" stripekit)

if(TARGET stripekit_libsodium_external OR TARGET drogon)
    message(FATAL_ERROR "StripeKit fetched a dependency despite a parent-provided target.")
endif()
get_target_property(core_dependencies stripekit LINK_LIBRARIES)
get_target_property(adapter_dependencies stripekit_drogon LINK_LIBRARIES)
if(NOT "sodium" IN_LIST core_dependencies OR
   NOT "Drogon::Drogon" IN_LIST adapter_dependencies)
    message(FATAL_ERROR "StripeKit did not link the parent-provided targets.")
endif()
