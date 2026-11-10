# Whether the development preview binary carries the camera settings write path (#416 stage B, ADR-0038).
# -DBINARY=<the preview exe> -DEXPECT=absent|present
# A build with A0_BUILD_WPD_SETTINGS_WRITE=OFF must carry none of the write channel's strings; a build with it
# ON must carry them (so the check itself is seen to find them). Reads the file only; runs nothing.
if(NOT DEFINED BINARY OR NOT EXISTS "${BINARY}")
    message(FATAL_ERROR "settings write binary check: missing BINARY")
endif()
if(NOT EXPECT STREQUAL "absent" AND NOT EXPECT STREQUAL "present")
    message(FATAL_ERROR "settings write binary check: EXPECT must be absent or present")
endif()
# The write begin's error text, the journal line prefix, the build's startup line and its camera-free smoke.
file(STRINGS "${BINARY}" hits REGEX "settings write begin|a0sw1|settings_write_build=|--settings-write-ui-smoke")
list(LENGTH hits count)
if(EXPECT STREQUAL "absent" AND count GREATER 0)
    message(FATAL_ERROR "settings write binary check: an OFF build carries ${count} settings write strings")
endif()
if(EXPECT STREQUAL "present" AND count EQUAL 0)
    message(FATAL_ERROR "settings write binary check: an ON build carries no settings write string")
endif()
message(STATUS "settings write binary check: expect=${EXPECT} strings=${count}")
