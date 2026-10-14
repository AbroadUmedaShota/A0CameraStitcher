cmake_minimum_required(VERSION 3.24)
# Configure-time policy of the development-only fiducial detector (Issue #310):
# OpenCV may be reached only by the targets named ALLOWED, and the detector
# itself must not reach the generator or the product stitch.
if(NOT DEFINED ISOLATION_MODULE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "ISOLATION_MODULE and TEST_ROOT are required")
endif()
set(case_count 0)
function(check_case name reject body)
    # Numbered case directories keep Windows cache paths short.
    set(case_dir "${TEST_ROOT}/${case_count}")
    set(source_dir "${case_dir}/s")
    file(MAKE_DIRECTORY "${source_dir}")
    file(WRITE "${source_dir}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.24)\nproject(fiducial_detector_isolation NONE)\n"
        "include([==[${ISOLATION_MODULE}]==])\n"
        "add_library(subject INTERFACE)\nadd_library(bridge INTERFACE)\nadd_library(other INTERFACE)\n${body}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${source_dir}" -B "${case_dir}/b"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(reject)
        if(result EQUAL 0 OR NOT "${output}${errors}" MATCHES "must not (link|compile|use)")
            message(FATAL_ERROR "${name}: rejection was not the dependency policy (${result})")
        endif()
    elseif(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: allowed configuration rejected (${result}): ${output}${errors}")
    endif()
    math(EXPR count "${case_count} + 1")
    set(case_count ${count} PARENT_SCOPE)
endfunction()

# --- OpenCV: allowed only in the listed targets.
set(defer_opencv "a0_defer_to_end_of_configure(a0_assert_opencv_only_in_allowed_targets ALLOWED subject)")
check_case(no_opencv_anywhere FALSE "${defer_opencv}")
check_case(allowed_target_links_opencv FALSE
    "${defer_opencv}\ntarget_link_libraries(subject INTERFACE opencv_world4140)")
check_case(allowed_target_includes_opencv FALSE
    "${defer_opencv}\ntarget_include_directories(subject INTERFACE [==[C:/tools/opencv/build/include]==])")
check_case(allowed_target_imported_opencv FALSE
    "${defer_opencv}\nadd_library(OpenCV::Core INTERFACE IMPORTED)\ntarget_link_libraries(subject INTERFACE OpenCV::Core)")
foreach(library opencv_world4140 libopencv_imgproc cv2)
    check_case("direct_${library}" TRUE
        "${defer_opencv}\ntarget_link_libraries(other INTERFACE ${library})")
    check_case("transitive_${library}" TRUE
        "${defer_opencv}\ntarget_link_libraries(other INTERFACE bridge)\ntarget_link_libraries(bridge INTERFACE $<$<CONFIG:Release>:${library}>)")
endforeach()
check_case(imported_opencv TRUE
    "${defer_opencv}\nadd_library(OpenCV::Core INTERFACE IMPORTED)\ntarget_link_libraries(other INTERFACE OpenCV::Core)")
check_case(link_option_opencv TRUE
    "${defer_opencv}\ntarget_link_options(other INTERFACE /DEFAULTLIB:opencv_world4140.lib)")
check_case(include_directory_opencv TRUE
    "${defer_opencv}\ntarget_include_directories(other INTERFACE [==[C:/tools/OpenCV-4.14.0/opencv/build/include]==])")
check_case(allowed_does_not_cover_others TRUE
    "${defer_opencv}\ntarget_link_libraries(subject INTERFACE opencv_world4140)\ntarget_link_libraries(other INTERFACE subject)")

# --- The detector must not reach the generator or the product stitch.
set(defer_independent "a0_defer_to_end_of_configure(a0_assert_fiducial_detector_independent subject)")
check_case(detector_allowed_dependencies FALSE
    "${defer_independent}\ntarget_link_libraries(subject INTERFACE a0_m2_calibration_observation_session bcrypt opencv_world4140)")
foreach(library a0_m2_synthetic_pair a0_m2_render a0_m2_offline_stitcher a0_m2_rig_profile_v2)
    check_case("detector_direct_${library}" TRUE
        "${defer_independent}\ntarget_link_libraries(subject INTERFACE ${library})")
endforeach()
check_case(detector_transitive_synthetic_pair TRUE
    "${defer_independent}\ntarget_link_libraries(subject INTERFACE bridge)\ntarget_link_libraries(bridge INTERFACE a0_m2_synthetic_pair)")
check_case(detector_source_synthetic_pair TRUE
    "${defer_independent}\ntarget_link_libraries(subject INTERFACE bridge)\nset_property(TARGET bridge PROPERTY INTERFACE_SOURCES src/m2/synthetic_pair_chart.cpp)")

if(NOT case_count EQUAL 21)
    message(FATAL_ERROR "Fiducial detector isolation ran ${case_count} cases, expected 21")
endif()
message(STATUS "Fiducial detector isolation: ${case_count} cases passed")
