# The one-shot capture (--capture-a / --capture-b) of a build that carries the camera settings write path
# (#416 stage B, ADR-0038) reaches the shutter only through RunCaptureOnce, which asks the capture hold first.
# -DSOURCE=<wpd_dual_preview_ui.hpp>. Reads the file only; runs nothing.
# Each line of the one-shot capture that names both `capture_once_alias` and `SaveStillOnce(` and sits in the branch
# for A0_ENABLE_WPD_SETTINGS_WRITE must call RunCaptureOnce on the same line. A caller that skips the call (and takes
# the picture directly) fails here. At least one such line must exist, so that the check is seen to find it.
if(NOT DEFINED SOURCE OR NOT EXISTS "${SOURCE}")
    message(FATAL_ERROR "capture once source check: missing SOURCE")
endif()
file(STRINGS "${SOURCE}" lines)
set(last_directive "")
set(found 0)
set(bypassed 0)
foreach(line IN LISTS lines)
    if(line MATCHES "^[ \t]*#")
        set(last_directive "${line}")
    endif()
    if(line MATCHES "capture_once_alias" AND line MATCHES "SaveStillOnce\\(" AND last_directive MATCHES "defined\\(A0_ENABLE_WPD_SETTINGS_WRITE\\)")
        math(EXPR found "${found} + 1")
        if(NOT line MATCHES "RunCaptureOnce\\(")
            math(EXPR bypassed "${bypassed} + 1")
        endif()
    endif()
endforeach()
if(found EQUAL 0)
    message(FATAL_ERROR "capture once source check: the one-shot capture line of the write build was not found")
endif()
if(bypassed GREATER 0)
    message(FATAL_ERROR "capture once source check: ${bypassed} one-shot capture line(s) reach SaveStillOnce without RunCaptureOnce")
endif()
message(STATUS "capture once source check: lines=${found} bypassed=0")
