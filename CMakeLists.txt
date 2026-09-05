# asura_wrath_recomp - ReXGlue Recompiled Project
cmake_minimum_required(VERSION 3.25)
project(asura_wrath_recomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(
    -O3
    -ffast-math
    -funroll-loops
    -Wno-nan-infinity-disabled
    -Wno-c++98-compat
    -Wno-c++98-compat-pedantic
    -Wno-reserved-identifier
    -Wno-unsafe-buffer-usage
    -Wno-global-constructors
    -DREXGLUE_PROFILE_GUEST_FUNCTIONS
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_compile_options(-mssse3 -mavx2 -mfma)
endif()

if(NOT REXSDK_DIR)
    set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/rexglue" CACHE PATH "Path to rexglue-sdk source tree")
endif()

if(NOT DEFINED REX_PLATFORM)
    if(WIN32)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|x86")
            set(REX_PLATFORM "win-amd64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
            set(REX_PLATFORM "win-arm64")
        endif()
    elseif(UNIX AND NOT APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|x86")
            set(REX_PLATFORM "linux-amd64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
            set(REX_PLATFORM "linux-arm64")
        endif()
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|x86")
            set(REX_PLATFORM "mac-amd64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64")
            set(REX_PLATFORM "mac-arm64")
        endif()
    endif()
endif()

include(generated/rexglue.cmake)

set(ASURAWRATH_SOURCES
    src/main.cpp
)

add_executable(asura_wrath_recomp ${ASURAWRATH_SOURCES})

if(UNIX AND NOT APPLE)
    target_link_options(asura_wrath_recomp PRIVATE "-Wl,-rpath,${REXSDK_DIR}/out/${REX_PLATFORM}:$ORIGIN/../../../tools/rexglue/out/${REX_PLATFORM}:$ORIGIN")
elseif(APPLE)
    target_link_options(asura_wrath_recomp PRIVATE "-Wl,-rpath,${REXSDK_DIR}/out/${REX_PLATFORM}:@executable_path/../../../tools/rexglue/out/${REX_PLATFORM}:@executable_path")
endif()

set_target_properties(asura_wrath_recomp PROPERTIES OUTPUT_NAME "asura_wrath_recomp")

set(REXGLUE_RECOMP_DEBUG_INFO "none" CACHE STRING "Debug info level" FORCE)

rexglue_setup_target(asura_wrath_recomp GPU_PLUGINS xenos)
target_include_directories(asura_wrath_recomp PRIVATE ${REXSDK_DIR}/thirdparty/imgui ${CMAKE_CURRENT_SOURCE_DIR}/src)

