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

    # Add new documentation pages and public headers here so Doxygen picks them
    # up and the target rebuilds when they change.
    set(stripekit_doc_pages
        docs/MainPage.dox
        docs/Topics.dox
        docs/Overview.md
        docs/Quickstart.md
        docs/ProductsAndPrices.md
        docs/WritingAnAdapter.md
        docs/versioning.md
    )

    set(stripekit_doc_headers
        include/StripeKit/ApiConstants.h
        include/StripeKit/Async.h
        include/StripeKit/Callback.h
        include/StripeKit/Client.h
        include/StripeKit/Errors.h
        include/StripeKit/Models.h
        include/StripeKit/RequestOptions.h
        include/StripeKit/StripeKit.h
        src/core/include/StripeKit/Events.h
        src/core/include/StripeKit/HTTPClient.h
        src/core/include/StripeKit/Logging.h
        src/core/include/StripeKit/SqliteWebhookDedupStore.h
        src/core/include/StripeKit/StripeApiClient.h
        src/core/include/StripeKit/WebhookDedupStore.h
        src/core/include/StripeKit/WebhookProcessor.h
        src/core/include/StripeKit/WebhookVerifier.h
        src/core/include/StripeKit/Webhooks.h
        src/adapters/drogon/include/StripeKit/Adapters/Drogon.h
    )

    set(stripekit_doc_sources "")
    foreach(stripekit_doc_source IN LISTS stripekit_doc_pages stripekit_doc_headers)
        list(APPEND stripekit_doc_sources ${CMAKE_CURRENT_SOURCE_DIR}/${stripekit_doc_source})
    endforeach()

    list(JOIN stripekit_doc_sources " \\\n                         " STRIPEKIT_DOXYGEN_INPUT)

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

    add_custom_command(
        OUTPUT ${STRIPEKIT_DOXYGEN_OUTPUT_DIR}/html/index.html
        COMMAND ${DOXYGEN_EXECUTABLE} ${stripekit_doxyfile}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        DEPENDS ${stripekit_doxyfile} ${stripekit_doc_sources}
        COMMENT "Generating the StripeKit API reference with Doxygen"
        VERBATIM
    )

    add_custom_target(stripekit-docs ALL
        DEPENDS ${STRIPEKIT_DOXYGEN_OUTPUT_DIR}/html/index.html
    )
endif()
