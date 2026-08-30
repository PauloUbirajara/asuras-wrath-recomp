# asura_wrath_recomp - ReXGlue Recompiled Project
cmake_minimum_required(VERSION 3.25)
project(asura_wrath_recomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

set(ASURAWRATH_SOURCES
    src/main.cpp
)

add_executable(asura_wrath_recomp ${ASURAWRATH_SOURCES})
target_link_options(asura_wrath_recomp PRIVATE "-Wl,-rpath,${REXSDK_DIR}/out/linux-amd64:$ORIGIN/tools/rexglue/out/linux-amd64:$ORIGIN")

set_target_properties(asura_wrath_recomp PROPERTIES OUTPUT_NAME "asura_wrath_recomp")

set(REXGLUE_RECOMP_DEBUG_INFO "none" CACHE STRING "Debug info level" FORCE)

add_compile_options(
    -O3
    -march=native
    -mavx2
    -mfma
    -ffast-math
    -funroll-loops
    -Wno-nan-infinity-disabled
    -DREXGLUE_PROFILE_GUEST_FUNCTIONS
)

rexglue_setup_target(asura_wrath_recomp GPU_PLUGINS xenos)
target_include_directories(asura_wrath_recomp PRIVATE ${REXSDK_DIR}/thirdparty/imgui ${CMAKE_CURRENT_SOURCE_DIR}/src)
