cmake_minimum_required(VERSION 3.20)
project(NvD3D12NativePSOCache_BugRepro LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This repro requires Windows and the Windows SDK.")
endif()

add_executable(native_pso_repro main.cpp)
target_compile_features(native_pso_repro PRIVATE cxx_std_17)
target_compile_definitions(native_pso_repro PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(native_pso_repro PRIVATE d3d12 dxgi d3dcompiler)
if(MSVC)
    target_compile_options(native_pso_repro PRIVATE /W4 /permissive- /utf-8)
endif()

set(REPRO_ADAPTER "0" CACHE STRING "DXGI adapter index; see native_pso_repro --list-adapters")
option(REPRO_DEBUG_LAYER "Enable the optional D3D12 debug layer in the repro matrix" OFF)
add_custom_target(repro
    COMMAND "${CMAKE_COMMAND}"
        "-DREPRO_EXE=$<TARGET_FILE:native_pso_repro>"
        "-DADAPTER=${REPRO_ADAPTER}"
        "-DCACHE_FILE=${CMAKE_CURRENT_BINARY_DIR}/pipeline-library-${REPRO_ADAPTER}.bin"
        "-DDEBUG_LAYER=${REPRO_DEBUG_LAYER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_repro.cmake"
    DEPENDS native_pso_repro
    USES_TERMINAL
    VERBATIM
)
