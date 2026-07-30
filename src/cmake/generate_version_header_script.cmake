# generate_version_header_script.cmake
#
# Script to generate a version header file.
#
# Do NOT use directly. Instead include(generate_version_header) in your script

if(DEFINED ENV{CI_JOB_ID})
    # Official Release Build
    # Use the build server's job ID if this is being built on a build server
    set(BUILD_ID $ENV{CI_JOB_ID})
else()
    # Internal Build
    string(TIMESTAMP BUILD_ID "%Y%m%dT%H%M%S%f-dev" UTC)    
endif()

string(TIMESTAMP BUILD_DATE "%Y-%m-%d" UTC)
string(TIMESTAMP BUILD_YEAR "%Y" UTC)

configure_file(${VERSION_TEMPLATE} ${VERSION_FILE})