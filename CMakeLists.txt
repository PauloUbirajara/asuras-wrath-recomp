# asura_wrath - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(asura_wrath LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CPU flags (-march=native or -msse4.1) are chosen by build.sh.

# Build the SDK from the pinned submodule. Project-side changes only; the
# SDK tree under thirdparty/ is kept unmodified.
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue" CACHE PATH "Path to rexglue-sdk source tree")

include(generated/rexglue.cmake)

# Sources
set(ASURAWRATH_SOURCES
    src/main.cpp
    src/ui/game_font.cpp
    src/ui/game_toast.cpp
)

if(WIN32)
    add_executable(asura_wrath WIN32 ${ASURAWRATH_SOURCES})
else()
    add_executable(asura_wrath ${ASURAWRATH_SOURCES})
endif()

# mantra: native-renderer GPU plugin, built from this repo (src/mantra_renderer).
add_subdirectory(src/mantra_renderer)

# mantra_shadergen: offline shader translation (the launcher's shader step).
option(ASURA_BUILD_SHADERGEN "Build the mantra_shadergen tool" ON)
if(ASURA_BUILD_SHADERGEN AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(tools/mantra_shadergen)
  add_subdirectory(tools/xxx_unpack)  # .xxx -> UE3 package bodies (asset extraction)
endif()

rexglue_setup_target(asura_wrath GPU_PLUGINS xenos mantra)

# The SDK writes librexruntime.so into its own out/ dir; stage it next to the
# executable so the $ORIGIN RUNPATH (and the GPU plugins) find it.
if(UNIX AND NOT APPLE)
  add_custom_command(TARGET asura_wrath POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:rexruntime> $<TARGET_FILE_DIR:asura_wrath>
      VERBATIM)
endif()
