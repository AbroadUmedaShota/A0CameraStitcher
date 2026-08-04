cmake_minimum_required(VERSION 3.25)
project(A0CameraStitcherPhase0 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NIKON_D810_SDK_ROOT "" CACHE PATH "Ignored local path to the licensed Nikon D810 Camera Remote SDK")

set(A0_NIKON_SDK_AVAILABLE OFF)
if(NIKON_D810_SDK_ROOT)
    file(GLOB_RECURSE A0_NIKON_MAID_HEADERS LIST_DIRECTORIES FALSE
        "${NIKON_D810_SDK_ROOT}/Maid3.h"
        "${NIKON_D810_SDK_ROOT}/*/Maid3.h"
        "${NIKON_D810_SDK_ROOT}/*/*/Maid3.h"
        "${NIKON_D810_SDK_ROOT}/*/*/*/Maid3.h"
        "${NIKON_D810_SDK_ROOT}/*/*/*/*/Maid3.h"
        "${NIKON_D810_SDK_ROOT}/*/*/*/*/*/Maid3.h"
        "${NIKON_D810_SDK_ROOT}/*/*/*/*/*/*/Maid3.h")
    file(GLOB_RECURSE A0_NIKON_MODULES LIST_DIRECTORIES FALSE
        "${NIKON_D810_SDK_ROOT}/Type0014.md3"
        "${NIKON_D810_SDK_ROOT}/*/Type0014.md3"
        "${NIKON_D810_SDK_ROOT}/*/*/Type0014.md3"
        "${NIKON_D810_SDK_ROOT}/*/*/*/Type0014.md3"
        "${NIKON_D810_SDK_ROOT}/*/*/*/*/Type0014.md3"
        "${NIKON_D810_SDK_ROOT}/*/*/*/*/*/Type0014.md3"
        "${NIKON_D810_SDK_ROOT}/*/*/*/*/*/*/Type0014.md3")
    list(FILTER A0_NIKON_MAID_HEADERS INCLUDE REGEX "Module/Win/Header Files/Maid3\\.h$")
    list(FILTER A0_NIKON_MODULES INCLUDE REGEX "Module/Win/Binary Files/x64/Type0014\\.md3$")
    list(LENGTH A0_NIKON_MAID_HEADERS A0_NIKON_HEADER_COUNT)
    list(LENGTH A0_NIKON_MODULES A0_NIKON_MODULE_COUNT)
    if(A0_NIKON_HEADER_COUNT EQUAL 1 AND A0_NIKON_MODULE_COUNT EQUAL 1)
        list(GET A0_NIKON_MAID_HEADERS 0 A0_NIKON_MAID_HEADER)
        list(GET A0_NIKON_MODULES 0 A0_NIKON_MODULE)
        get_filename_component(A0_NIKON_INCLUDE_DIR "${A0_NIKON_MAID_HEADER}" DIRECTORY)
        get_filename_component(A0_NIKON_BINARY_DIR "${A0_NIKON_MODULE}" DIRECTORY)
        if(EXISTS "${A0_NIKON_INCLUDE_DIR}/Maid3d1.h"
                AND EXISTS "${A0_NIKON_INCLUDE_DIR}/NkTypes.h"
                AND EXISTS "${A0_NIKON_BINARY_DIR}/NkdPTP.dll"
                AND EXISTS "${A0_NIKON_BINARY_DIR}/NkRoyalmile.dll"
                AND EXISTS "${A0_NIKON_BINARY_DIR}/dnssd.dll")
            set(A0_NIKON_SDK_AVAILABLE ON)
        endif()
    endif()
endif()

add_library(a0_phase0_core
    src/phase0/phase0.cpp
    src/phase0/fake_camera_transport.cpp
    src/phase0/nikon_sdk_transport.cpp
    src/phase0/wpd_transport.cpp
)
target_include_directories(a0_phase0_core PUBLIC src/phase0/include)
target_compile_definitions(a0_phase0_core PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(a0_phase0_core PRIVATE bcrypt ole32 PortableDeviceGuids)

if(A0_NIKON_SDK_AVAILABLE)
    file(TO_CMAKE_PATH "${A0_NIKON_MODULE}" A0_NIKON_MODULE_NORMALIZED)
    target_include_directories(a0_phase0_core PRIVATE "${A0_NIKON_INCLUDE_DIR}")
    target_compile_definitions(a0_phase0_core PRIVATE
        A0_NIKON_SDK_AVAILABLE=1
        A0_NIKON_SDK_MODULE_PATH="${A0_NIKON_MODULE_NORMALIZED}")
    message(STATUS "Nikon D810 licensed adapter enabled from ignored local SDK root")
elseif(NIKON_D810_SDK_ROOT)
    message(WARNING "NIKON_D810_SDK_ROOT is incomplete or ambiguous; building the gated stub")
endif()

if(MSVC)
    target_compile_options(a0_phase0_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(a0_phase0_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(A0CameraStitcher.Phase0 src/phase0/main.cpp)
target_link_libraries(A0CameraStitcher.Phase0 PRIVATE a0_phase0_core)
if(MSVC)
    target_compile_options(A0CameraStitcher.Phase0 PRIVATE /W4 /permissive- /EHsc)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(a0_phase0_tests tests/phase0_tests.cpp)
    target_link_libraries(a0_phase0_tests PRIVATE a0_phase0_core)
    if(MSVC)
        target_compile_options(a0_phase0_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME phase0_contracts COMMAND a0_phase0_tests)
endif()
