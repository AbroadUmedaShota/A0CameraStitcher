# Whether the development preview binary carries the recalibration button (#434).
# -DBINARY=<the preview exe> -DEXPECT=absent|present
# A build with A0_BUILD_WPD_RECALIBRATION=OFF must carry no entrance to it: none of its command-line entries, log keys or sentences, in the
# 8-bit form (ASCII and UTF-8) or in the wide form (UTF-16, which the window's own texts have). A build with it ON must carry them, so that
# the check is seen to find them. Reads the file only; runs nothing.
if(NOT DEFINED BINARY OR NOT EXISTS "${BINARY}")
    message(FATAL_ERROR "recalibration binary check: missing BINARY")
endif()
if(NOT EXPECT STREQUAL "absent" AND NOT EXPECT STREQUAL "present")
    message(FATAL_ERROR "recalibration binary check: EXPECT must be absent or present")
endif()

# ASCII: the camera-free smoke's entry, the log keys of the screens, and the kind of the pair that a calibration's shot records.
file(STRINGS "${BINARY}" ascii_hits REGEX "recalibration-ui-smoke|recalibration_ui |recalibration_capture_|wpd-recalibration-pair|recalibration_unchecked_mark")
list(LENGTH ascii_hits ascii_count)

# Wide and UTF-8 forms of sentences that only this change shows (the hex of the code units, read from the file as hex):
#   校正をやり直す (the entrance), 未確認の校正 (the mark), 撮影して校正する (the shot).
file(READ "${BINARY}" binary_hex HEX)
set(wide_markers
    "2168636b923084308a30f4765930"                       # 校正をやり直す  UTF-16LE
    "2a67ba788d8a6e302168636b"                           # 未確認の校正    UTF-16LE
    "e6a0a1e6ada3e38292e38284e3828ae79bb4e38199"         # 校正をやり直す  UTF-8
    "e69caae7a2bae8aa8de381aee6a0a1e6ada3")              # 未確認の校正    UTF-8
set(wide_count 0)
foreach(marker IN LISTS wide_markers)
    string(FIND "${binary_hex}" "${marker}" at)
    if(NOT at EQUAL -1)
        math(EXPR wide_count "${wide_count} + 1")
    endif()
endforeach()

if(EXPECT STREQUAL "absent")
    if(ascii_count GREATER 0 OR wide_count GREATER 0)
        message(FATAL_ERROR "recalibration binary check: an OFF build carries the recalibration (ascii=${ascii_count} wide=${wide_count})")
    endif()
else()
    if(ascii_count EQUAL 0 OR wide_count EQUAL 0)
        message(FATAL_ERROR "recalibration binary check: an ON build lacks it (ascii=${ascii_count} wide=${wide_count})")
    endif()
endif()
message(STATUS "recalibration binary check: expect=${EXPECT} ascii=${ascii_count} wide=${wide_count}")
