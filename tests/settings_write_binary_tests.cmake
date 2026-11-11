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

# The entries of the confirmation test build (a table of the test's own) must not be in the preview itself: the
# product exe carries none of them (TEST_STRINGS defaults to absent). The test binary is checked with
# -DTEST_STRINGS=present, so that this check is seen to find them.
if(NOT DEFINED TEST_STRINGS)
    set(TEST_STRINGS absent)
endif()
file(STRINGS "${BINARY}" test_hits REGEX "settings-write-confirm-smoke|settings_write_test_build=|settings_write_confirm_smoke=")
list(LENGTH test_hits test_count)
if(TEST_STRINGS STREQUAL "absent" AND test_count GREATER 0)
    message(FATAL_ERROR "settings write binary check: this binary carries ${test_count} confirmation test strings")
endif()
if(TEST_STRINGS STREQUAL "present" AND test_count EQUAL 0)
    message(FATAL_ERROR "settings write binary check: the test binary carries no confirmation test string")
endif()
message(STATUS "settings write binary check: test_strings=${TEST_STRINGS} count=${test_count}")
