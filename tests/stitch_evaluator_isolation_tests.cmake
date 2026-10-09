cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED ISOLATION_MODULE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "ISOLATION_MODULE and TEST_ROOT are required")
endif()
function(check_case name reject body)
    set(source_dir "${TEST_ROOT}/${name}/source")
    file(MAKE_DIRECTORY "${source_dir}")
    file(WRITE "${source_dir}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.24)\nproject(evaluator_isolation NONE)\n"
        "include([==[${ISOLATION_MODULE}]==])\n"
        "add_library(subject INTERFACE)\nadd_library(bridge INTERFACE)\n"
        "a0_defer_to_end_of_configure(a0_assert_no_stitch_evaluator_dependency subject)\n${body}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${source_dir}" -B "${TEST_ROOT}/${name}/build"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(reject)
        if(result EQUAL 0 OR NOT "${output}${errors}" MATCHES "must not (link|compile)")
            message(FATAL_ERROR "${name}: rejection was not the dependency policy (${result})")
        endif()
    elseif(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: valid independent IO rejected (${result}): ${output}${errors}")
    endif()
endfunction()
check_case(io_allowed FALSE "target_link_libraries(subject INTERFACE ole32 windowscodecs bcrypt)")
foreach(library a0_m2_render a0_m2_offline_stitcher a0_m2_synthetic_pair a0_m2_rig_profile_v2)
    check_case("direct_${library}" TRUE "target_link_libraries(subject INTERFACE ${library})")
    check_case("transitive_${library}" TRUE
        "target_link_libraries(subject INTERFACE bridge)\ntarget_link_libraries(bridge INTERFACE $<$<CONFIG:Release>:${library}>)")
endforeach()
foreach(source render.cpp document_render.cpp offline_stitcher.cpp stitch_job_manifest.cpp synthetic_pair_geometry.cpp rig_profile_v2.cpp stitch_eval_main.cpp)
    check_case("source_${source}" TRUE "set_property(TARGET subject PROPERTY INTERFACE_SOURCES src/m2/${source})")
endforeach()
check_case(link_option TRUE "target_link_libraries(subject INTERFACE bridge)\ntarget_link_options(bridge INTERFACE /DEFAULTLIB:a0_m2_render.lib)")
check_case(independent_core FALSE "set_property(TARGET subject PROPERTY INTERFACE_SOURCES src/m2/stitch_evaluation.cpp)")
message(STATUS "Evaluator isolation: 18 cases passed")
