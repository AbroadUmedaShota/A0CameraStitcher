# Link-isolation checks for pure-computation libraries.
#
# a0_assert_no_link_dependency(<target> [FORBIDDEN_LIBRARIES <names>...]
#   [FORBIDDEN_SOURCES <regexes>...])
#   Fails configuration when <target>, or anything it reaches through
#   LINK_LIBRARIES / INTERFACE_LINK_LIBRARIES / LINK_OPTIONS /
#   INTERFACE_LINK_OPTIONS (transitively), names one of the forbidden system
#   libraries (ole32, windowscodecs, bcrypt). Used to keep a0_m2_render free of
#   Win32 / COM / WIC / BCrypt dependencies.
#   Explicit library/source lists use the same transitive walk to keep the
#   ground-truth generator independent of the product renderer and stitcher.
#
#   The check is deliberately conservative. Every entry is split into words at
#   generator-expression delimiters, ';', ',', ':', '/', '\' and spaces, and each
#   word (with a trailing .lib/.dll/.a and a leading -l removed) is compared with
#   the forbidden list, so "$<$<CONFIG:Release>:bcrypt>", "/DEFAULTLIB:bcrypt.lib"
#   and "-lbcrypt" are all caught. A word that names a target is followed.
#   A false positive is preferred over a missed dependency.
#
# a0_defer_to_end_of_configure(<function> [<args>...])
#   Runs <function> with <args> at the end of the top-level directory, once every
#   target_link_libraries / target_link_options call has been made. Call the
#   assertions through this, never directly: a direct call only sees the link
#   state at that line, so a dependency added later would pass. Other
#   assertion functions that read target properties (for example a
#   "no product stitcher dependency" check) can be registered the same way.
#   Arguments are fixed when this is called; the properties they refer to are
#   read when the deferred call runs.

function(a0_assert_no_link_dependency target)
    cmake_parse_arguments(PARSE_ARGV 1 isolation "" "" "FORBIDDEN_LIBRARIES;FORBIDDEN_SOURCES")
    if(isolation_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown isolation arguments: ${isolation_UNPARSED_ARGUMENTS}")
    endif()
    if(DEFINED isolation_FORBIDDEN_LIBRARIES)
        set(forbidden ${isolation_FORBIDDEN_LIBRARIES})
    else()
        set(forbidden ole32 windowscodecs bcrypt)
    endif()
    set(pending "${target}")
    set(seen "")
    while(NOT "${pending}" STREQUAL "")
        list(POP_FRONT pending current)
        if("${current}" STREQUAL "" OR "${current}" IN_LIST seen)
            continue()
        endif()
        list(APPEND seen "${current}")
        string(TOLOWER "${current}" current_lower)
        if("${current_lower}" IN_LIST forbidden)
            message(FATAL_ERROR "${target} must not link ${current}")
        endif()
        foreach(property SOURCES INTERFACE_SOURCES)
            get_target_property(entries "${current}" ${property})
            foreach(source IN LISTS entries)
                string(REPLACE "\\" "/" normalized_source "${source}")
                string(TOLOWER "${normalized_source}" normalized_source)
                string(REGEX REPLACE "[$<>,: \"']" ";" source_words "${normalized_source}")
                foreach(word IN LISTS source_words)
                    foreach(pattern IN LISTS isolation_FORBIDDEN_SOURCES)
                        if("/${word}" MATCHES "${pattern}")
                            message(FATAL_ERROR "${target} must not compile product source ${source} (via ${current})")
                        endif()
                    endforeach()
                endforeach()
            endforeach()
        endforeach()
        foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES LINK_OPTIONS INTERFACE_LINK_OPTIONS)
            get_target_property(entries "${current}" ${property})
            if("${entries}" STREQUAL "" OR "${entries}" MATCHES "NOTFOUND$")
                continue()
            endif()
            foreach(entry IN LISTS entries)
                string(REPLACE "\\" ";" normalized "${entry}")
                string(REGEX REPLACE "[$<>,()/ \"']" ";" normalized "${normalized}")
                set(words "")
                foreach(coarse IN LISTS normalized)
                    string(REPLACE ":" ";" fine "${coarse}")
                    list(APPEND words "${coarse}" ${fine})
                endforeach()
                foreach(word IN LISTS words)
                    if("${word}" STREQUAL "")
                        continue()
                    endif()
                    string(TOLOWER "${word}" lowered)
                    string(REGEX REPLACE "^-l" "" without_flag "${lowered}")
                    foreach(candidate "${lowered}" "${without_flag}")
                        string(REGEX REPLACE "\\.(lib|dll|a)$" "" library_name "${candidate}")
                        if("${library_name}" IN_LIST forbidden)
                            message(FATAL_ERROR
                                "${target} must not link ${library_name} "
                                "(found in ${property} of ${current}: ${entry})")
                        endif()
                    endforeach()
                    if(TARGET "${word}")
                        list(APPEND pending "${word}")
                    endif()
                endforeach()
            endforeach()
        endforeach()
    endwhile()
endfunction()

# Actual policy shared by generator targets and configure-only regression
# tests. Both legacy and document-plane warp/blend must stay out of the
# independent ground-truth image generator.
function(a0_assert_no_product_stitch_dependency target)
    a0_assert_no_link_dependency("${target}"
        FORBIDDEN_LIBRARIES a0_m2_render a0_m2_offline_stitcher
        FORBIDDEN_SOURCES "/render\\.cpp$" "/document_render\\.cpp$" "offline_stitcher" "stitch_job_manifest")
endfunction()

# An image evaluator may consume independent oracle data, never the code that
# produces the pixels under examination (Issue #275). IO/system dependencies
# are checked separately for its pure numerical core.
function(a0_assert_no_stitch_evaluator_dependency target)
    a0_assert_no_link_dependency("${target}"
        FORBIDDEN_LIBRARIES a0_m2_render a0_m2_offline_stitcher a0_m2_synthetic_pair a0_m2_rig_profile_v2
        FORBIDDEN_SOURCES "/render\\.cpp$" "/document_render\\.cpp$" "offline_stitcher"
            "stitch_job_manifest" "synthetic_pair" "rig_profile_v2" "stitch_eval_main")
endfunction()

function(a0_defer_to_end_of_configure function_name)
    # DEFER evaluates ${...} in its arguments when the call runs, after this
    # function's scope is gone, so the arguments are expanded here first.
    set(code "cmake_language(DEFER DIRECTORY [==[${CMAKE_SOURCE_DIR}]==] CALL [==[${function_name}]==]")
    foreach(argument IN LISTS ARGN)
        string(APPEND code " [==[${argument}]==]")
    endforeach()
    string(APPEND code ")")
    cmake_language(EVAL CODE "${code}")
endfunction()
