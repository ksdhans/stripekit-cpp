# generate_version_header.cmake
#
# Generates a version header file for the current project.

set(VERSION_HEADER stripekit_rev.h)
set(VERSION_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/include)
set(VERSION_TEMPLATE ${CMAKE_CURRENT_LIST_DIR}/../Version.h.in)

add_custom_target(generate_${PROJECT_NAME}_version_header ALL
    COMMAND ${CMAKE_COMMAND}
        -DPROJECT_NAME=${PROJECT_NAME}
        -DPROJECT_VERSION=${PROJECT_VERSION}
        -DPROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        -DPROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        -DPROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        -DVERSION_TEMPLATE=${VERSION_TEMPLATE}
        -DVERSION_FILE=${VERSION_OUT_DIR}/${VERSION_HEADER}
        -P ${CMAKE_CURRENT_LIST_DIR}/generate_version_header_script.cmake
    COMMENT "Generating version header"
    BYPRODUCTS ${VERSION_OUT_DIR}/${VERSION_HEADER}
)

include_directories(${VERSION_OUT_DIR})
