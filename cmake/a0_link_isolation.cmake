# Link-isolation checks for pure-computation libraries.
#
# a0_assert_no_link_dependency(<target>)
#   Fails configuration when <target>, or anything it reaches through
#   LINK_LIBRARIES / INTERFACE_LINK_LIBRARIES / LINK_OPTIONS /
#   INTERFACE_LINK_OPTIONS (transitively), names one of the forbidden system
#   libraries (ole32, windowscodecs, bcrypt). Used to keep a0_m2_render free of
#   Win32 / COM / WIC / BCrypt dependencies.
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
    set(forbidden ole32 windowscodecs bcrypt)
    set(pending "${target}")
    set(seen "")
    while(NOT "${pending}" STREQUAL "")
        list(POP_FRONT pending current)
        if("${current}" STREQUAL "" OR "${current}" IN_LIST seen)
            continue()
        endif()
        list(APPEND seen "${current}")
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
