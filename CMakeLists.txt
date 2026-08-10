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
    src/phase0/cli_safety.cpp
    src/phase0/hardware_camera_agent.cpp
    src/phase0/hardware_camera_agent_pipe.cpp
    src/phase0/phase0.cpp
    src/phase0/fake_camera_transport.cpp
    src/phase0/hardware_process_lease.cpp
    src/phase0/nikon_sdk_transport.cpp
    src/phase0/wpd_transport.cpp
)
target_include_directories(a0_phase0_core PUBLIC src/phase0/include)
target_compile_definitions(a0_phase0_core PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(a0_phase0_core PRIVATE advapi32 bcrypt ole32 PortableDeviceGuids)

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

add_executable(A0CameraStitcher.CameraAgent
    src/phase0/hardware_camera_agent_main.cpp)
target_link_libraries(A0CameraStitcher.CameraAgent PRIVATE a0_phase0_core)
target_compile_definitions(A0CameraStitcher.CameraAgent PRIVATE UNICODE _UNICODE NOMINMAX)
if(MSVC)
    target_compile_options(A0CameraStitcher.CameraAgent PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(A0CameraStitcher.CameraAgent PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(a0_m2_optical
    src/m2/optical_planner.cpp
)
target_include_directories(a0_m2_optical PUBLIC src/m2/include)
if(MSVC)
    target_compile_options(a0_m2_optical PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(a0_m2_optical PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(a0_m2_setup
    src/m2/setup_assessment.cpp
)
target_include_directories(a0_m2_setup PUBLIC src/m2/include)
if(MSVC)
    target_compile_options(a0_m2_setup PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(a0_m2_setup PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(A0CameraStitcher.OpticalPlanner src/m2/main.cpp)
target_link_libraries(A0CameraStitcher.OpticalPlanner PRIVATE a0_m2_optical)
if(MSVC)
    target_compile_options(A0CameraStitcher.OpticalPlanner PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(A0CameraStitcher.OpticalPlanner PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(a0_phase0_tests tests/phase0_tests.cpp)
    target_link_libraries(a0_phase0_tests PRIVATE a0_phase0_core)
    if(MSVC)
        target_compile_options(a0_phase0_tests PRIVATE /W4 /permissive- /EHsc)
    endif()
    add_test(NAME phase0_contracts COMMAND a0_phase0_tests)

    add_executable(a0_phase0_cli_safety_tests tests/phase0_cli_safety_tests.cpp)
    target_link_libraries(a0_phase0_cli_safety_tests PRIVATE a0_phase0_core)
    if(MSVC)
        target_compile_options(a0_phase0_cli_safety_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(a0_phase0_cli_safety_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME phase0_cli_safety_contracts COMMAND a0_phase0_cli_safety_tests)

    add_executable(a0_hardware_camera_agent_tests tests/hardware_camera_agent_tests.cpp)
    target_link_libraries(a0_hardware_camera_agent_tests PRIVATE a0_phase0_core)
    if(MSVC)
        target_compile_options(a0_hardware_camera_agent_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(a0_hardware_camera_agent_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME hardware_camera_agent_contracts COMMAND a0_hardware_camera_agent_tests)

    add_executable(a0_m2_tests tests/m2_tests.cpp)
    target_link_libraries(a0_m2_tests PRIVATE a0_m2_optical)
    if(MSVC)
        target_compile_options(a0_m2_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(a0_m2_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME m2_optical_contracts COMMAND a0_m2_tests)

    add_executable(m2_setup_assessment_contracts tests/setup_assessment_tests.cpp)
    target_link_libraries(m2_setup_assessment_contracts PRIVATE a0_m2_setup)
    if(MSVC)
        target_compile_options(m2_setup_assessment_contracts PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(m2_setup_assessment_contracts PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME m2_setup_assessment_contracts COMMAND m2_setup_assessment_contracts)

    find_program(A0_POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)
    add_test(
        NAME m2_pregate_assets
        COMMAND "${A0_POWERSHELL_EXECUTABLE}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/Test-M2PreGateAssets.ps1")
endif()
