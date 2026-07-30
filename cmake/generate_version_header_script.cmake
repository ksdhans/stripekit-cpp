# generate_version_header_script.cmake
#
# Script to generate a version header file.
#
# Do not use directly. Instead include(cmake/generate_version_header.cmake).

if(DEFINED ENV{CI_JOB_ID})
    # Official release build on CI.
    set(BUILD_ID $ENV{CI_JOB_ID})
else()
    # Local or internal build.
    string(TIMESTAMP BUILD_ID "%Y%m%dT%H%M%S%f-dev" UTC)
endif()

string(TIMESTAMP BUILD_DATE "%Y-%m-%d" UTC)
string(TIMESTAMP BUILD_YEAR "%Y" UTC)

configure_file(${VERSION_TEMPLATE} ${VERSION_FILE})
