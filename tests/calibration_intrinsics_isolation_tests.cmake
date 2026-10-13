cmake_minimum_required(VERSION 3.24)
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
        "cmake_minimum_required(VERSION 3.24)\nproject(calibration_intrinsics_isolation NONE)\n"
        "include([==[${ISOLATION_MODULE}]==])\n"
        "add_library(subject INTERFACE)\nadd_library(bridge INTERFACE)\n"
        "a0_defer_to_end_of_configure(a0_assert_no_calibration_view_dependency subject)\n${body}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${source_dir}" -B "${case_dir}/b"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(reject)
        if(result EQUAL 0 OR NOT "${output}${errors}" MATCHES "must not (link|compile)")
            message(FATAL_ERROR "${name}: rejection was not the dependency policy (${result})")
        endif()
    elseif(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: pure intrinsics library rejected (${result})")
    endif()
    math(EXPR count "${case_count} + 1")
    set(case_count ${count} PARENT_SCOPE)
endfunction()
check_case(pure_allowed FALSE "target_link_libraries(subject INTERFACE bridge)\nset_property(TARGET bridge PROPERTY INTERFACE_SOURCES src/m2/calibration_intrinsics.cpp)")
# Names that are not targets are not followed by the checker, so this only shows that
# linking the two allowed libraries is not itself rejected.
check_case(allowed_dependencies FALSE "target_link_libraries(subject INTERFACE a0_m2_calibration_view_geometry a0_m2_calibration_observation_session)")
foreach(library a0_m2_synthetic_pair a0_m2_rig_profile_v2 bcrypt ole32 windowscodecs shell32 opencv_core opencv_calib3d opencv_world4120)
    check_case("direct_${library}" TRUE "target_link_libraries(subject INTERFACE ${library})")
    check_case("transitive_${library}" TRUE "target_link_libraries(subject INTERFACE bridge)\ntarget_link_libraries(bridge INTERFACE $<$<CONFIG:Release>:${library}>)")
endforeach()
check_case(imported_opencv TRUE "add_library(OpenCV::Core INTERFACE IMPORTED)\ntarget_link_libraries(subject INTERFACE OpenCV::Core)")
foreach(source synthetic_pair_geometry.cpp rig_profile_v2.cpp)
    string(REPLACE "." "_" name "${source}")
    check_case("transitive_source_${name}" TRUE "target_link_libraries(subject INTERFACE bridge)\nset_property(TARGET bridge PROPERTY INTERFACE_SOURCES src/m2/${source})")
endforeach()
if(NOT case_count EQUAL 23)
    message(FATAL_ERROR "Calibration intrinsics isolation ran ${case_count} cases, expected 23")
endif()
message(STATUS "Calibration intrinsics isolation: ${case_count} cases passed")
