cmake_minimum_required(VERSION 3.25)

project(sh3vr_host64 LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "sh3vr_host64 must be configured for 64-bit Windows.")
endif()

set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)

add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../thirds/OpenXR-SDK"
    "${CMAKE_CURRENT_BINARY_DIR}/openxr-sdk"
)

add_executable(sh3vr_host64
    sh3vr_host64/sh3vr_host64.cpp
    sh3vr_host64/runtime_selection.h
    ../sh3vr/common/shared_frame.h
)

target_compile_features(sh3vr_host64 PRIVATE cxx_std_17)
target_compile_definitions(sh3vr_host64 PRIVATE UNICODE _UNICODE)
target_include_directories(sh3vr_host64 PRIVATE ../sh3vr/common)
target_link_libraries(sh3vr_host64 PRIVATE
    OpenXR::openxr_loader
    d3d11
    d3dcompiler
    dxgi
    advapi32
)

if(MSVC)
    target_compile_options(sh3vr_host64 PRIVATE /W4 /permissive-)
endif()
