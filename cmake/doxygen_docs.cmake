# doxygen_docs.cmake
#
# Adds the "stripekit-docs" target, which generates the HTML API reference from
# the Doxygen comments in StripeKit's public headers.
#
# The reference is built by default for standalone StripeKit builds when Doxygen
# is installed, and is skipped otherwise. Set STRIPEKIT_BUILD_DOCS explicitly to
# override that, and pass -DSTRIPEKIT_BUILD_DOCS=ON to require the docs build to
# fail when Doxygen is missing.
#
# Output is written to ${CMAKE_BINARY_DIR}/docs/html/index.html.

find_package(Doxygen QUIET OPTIONAL_COMPONENTS dot)

set(stripekit_docs_default OFF)
if(STRIPEKIT_STANDALONE_PROJECT AND DOXYGEN_FOUND)
    set(stripekit_docs_default ON)
endif()

option(STRIPEKIT_BUILD_DOCS "Build the Doxygen API reference" ${stripekit_docs_default})

if(STRIPEKIT_BUILD_DOCS)
    if(NOT DOXYGEN_FOUND)
        message(FATAL_ERROR "STRIPEKIT_BUILD_DOCS is ON, but Doxygen was not found. Install Doxygen, or configure with -DSTRIPEKIT_BUILD_DOCS=OFF.")
    endif()

    set(STRIPEKIT_DOXYGEN_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/docs)

    set(stripekit_doxygen_inputs
        ${CMAKE_CURRENT_SOURCE_DIR}/README.md
        ${CMAKE_CURRENT_SOURCE_DIR}/include/StripeKit
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/include/StripeKit
        ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/drogon/include/StripeKit
    )
    list(JOIN stripekit_doxygen_inputs " \\\n                         " STRIPEKIT_DOXYGEN_INPUT)

    set(STRIPEKIT_DOXYGEN_HAVE_DOT NO)
    if(TARGET Doxygen::dot)
        set(STRIPEKIT_DOXYGEN_HAVE_DOT YES)
    endif()

    set(stripekit_doxyfile ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in
        ${stripekit_doxyfile}
        @ONLY
    )

    file(GLOB stripekit_doc_sources CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/include/StripeKit/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/include/StripeKit/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/drogon/include/StripeKit/Adapters/*.h
    )

    add_custom_command(
        OUTPUT ${STRIPEKIT_DOXYGEN_OUTPUT_DIR}/html/index.html
        COMMAND ${DOXYGEN_EXECUTABLE} ${stripekit_doxyfile}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        DEPENDS ${stripekit_doxyfile} ${stripekit_doc_sources} ${CMAKE_CURRENT_SOURCE_DIR}/README.md
        COMMENT "Generating the StripeKit API reference with Doxygen"
        VERBATIM
    )

    add_custom_target(stripekit-docs ALL
        DEPENDS ${STRIPEKIT_DOXYGEN_OUTPUT_DIR}/html/index.html
    )
endif()
