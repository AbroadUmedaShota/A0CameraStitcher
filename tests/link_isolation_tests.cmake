cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED ISOLATION_MODULE OR NOT DEFINED TEST_ROOT)
    message(FATAL_ERROR "ISOLATION_MODULE and TEST_ROOT are required")
endif()
get_filename_component(ISOLATION_MODULE "${ISOLATION_MODULE}" ABSOLUTE)
get_filename_component(TEST_ROOT "${TEST_ROOT}" ABSOLUTE)

# Tiny configure-only projects exercise the actual deferred dependency walk.
# Each has its own build directory, so positive and negative controls cannot
# reuse cached configure results. No compiler or product build is needed.
function(check_case name reject body)
    set(source_dir "${TEST_ROOT}/${name}/source")
    file(MAKE_DIRECTORY "${source_dir}")
    file(MAKE_DIRECTORY "${source_dir}/src/m2")
    file(WRITE "${source_dir}/src/m2/synthetic_pair_render.cpp" "")
    file(WRITE "${source_dir}/src/m2/document_render.cpp" "")
    file(WRITE "${source_dir}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.24)\nproject(isolation NONE)\n"
        "include([==[${ISOLATION_MODULE}]==])\n"
        "add_library(subject INTERFACE)\nadd_library(bridge INTERFACE)\n"
        "add_library(a0_m2_render INTERFACE)\nadd_library(a0_m2_offline_stitcher INTERFACE)\n"
        "a0_defer_to_end_of_configure(a0_assert_no_product_stitch_dependency subject)\n${body}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${source_dir}" -B "${TEST_ROOT}/${name}/build"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(reject)
        if(result EQUAL 0 OR NOT "${output}${errors}" MATCHES "must not (link|compile)")
            message(FATAL_ERROR "${name}: expected isolation rejection, got ${result}: ${output}${errors}")
        endif()
    elseif(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: expected acceptance, got ${result}: ${output}${errors}")
    endif()
    message(STATUS "${name}: passed")
endfunction()

check_case(allowed FALSE "target_link_libraries(subject INTERFACE ole32 windowscodecs bcrypt)")
check_case(direct TRUE "target_link_libraries(subject INTERFACE a0_m2_render)")
check_case(transitive TRUE "target_link_libraries(subject INTERFACE bridge)\ntarget_link_libraries(bridge INTERFACE a0_m2_render)")
check_case(interface_expression TRUE "target_link_libraries(subject INTERFACE bridge)\ntarget_link_libraries(bridge INTERFACE \"$<$<CONFIG:Release>:a0_m2_offline_stitcher>\")")
check_case(link_option TRUE "target_link_options(subject INTERFACE /DEFAULTLIB:a0_m2_render.lib)")
check_case(link_flag TRUE "target_link_libraries(subject INTERFACE -la0_m2_render)")
check_case(source TRUE "set_property(TARGET subject PROPERTY SOURCES src/m2/render.cpp)")
check_case(source_uppercase TRUE "set_property(TARGET subject PROPERTY SOURCES src/m2/RENDER.cpp)")
check_case(source_expression TRUE "set_property(TARGET subject PROPERTY SOURCES \"$<$<CONFIG:Release>:src/m2/render.cpp>\")")
check_case(source_windows_path TRUE "set_property(TARGET subject PROPERTY SOURCES [==[src\\m2\\render.cpp]==])")
check_case(interface_source TRUE "set_property(TARGET bridge PROPERTY INTERFACE_SOURCES src/m2/render.cpp)\ntarget_link_libraries(subject INTERFACE bridge)")
check_case(stitch_source TRUE "set_property(TARGET subject PROPERTY SOURCES src/m2/offline_stitcher.cpp)")
check_case(allowed_synthetic_source FALSE "set_property(TARGET subject PROPERTY SOURCES src/m2/synthetic_pair_render.cpp)")
check_case(document_source TRUE "set_property(TARGET subject PROPERTY SOURCES src/m2/document_render.cpp)")
check_case(document_interface_source TRUE "set_property(TARGET bridge PROPERTY INTERFACE_SOURCES src/m2/document_render.cpp)\ntarget_link_libraries(subject INTERFACE bridge)")

# Preserve the original pure-render rule, including flags and late additions.
check_case(system_default TRUE "a0_defer_to_end_of_configure(a0_assert_no_link_dependency bridge)\ntarget_link_options(bridge INTERFACE /DEFAULTLIB:bcrypt.lib)")
