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

# mantra (the default renderer) is Vulkan; the SDK leaves Vulkan off on
# Windows unless asked (it defaults to D3D12 there).
if(WIN32)
  set(REXGLUE_USE_VULKAN ON CACHE BOOL "Enable Vulkan graphics backend (mantra needs it)" FORCE)
endif()

# Building for Windows on Linux (cmake/windows-cross.cmake): the codegen step
# (generated/rexglue.cmake) runs $<TARGET_FILE:rex::rexglue>, a Windows .exe
# here. Run the Linux build's rexglue instead (./build.sh builds it). Only the
# codegen command is changed; every other add_custom_command passes through.
if(CMAKE_CROSSCOMPILING AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  set(REXGLUE_HOST_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue/out/linux-amd64/rexglue"
      CACHE FILEPATH "Linux rexglue tool for codegen in Windows cross builds")
  if(NOT EXISTS "${REXGLUE_HOST_TOOL}")
    message(FATAL_ERROR "Windows cross build: no Linux rexglue at ${REXGLUE_HOST_TOOL}. "
                        "Build for Linux first (./build.sh), or set REXGLUE_HOST_TOOL.")
  endif()
  # Forwards each argument as a bracket argument: re-expanding ${ARGV} would
  # split arguments that hold semicolons, such as $<...:a;b> expressions.
  function(add_custom_command)
    set(_call "_add_custom_command(")
    math(EXPR _last "${ARGC} - 1")
    foreach(_i RANGE ${_last})
      string(REPLACE "$<TARGET_FILE:rex::rexglue>" "${REXGLUE_HOST_TOOL}" _arg "${ARGV${_i}}")
      string(APPEND _call " [==[${_arg}]==]")
    endforeach()
    cmake_language(EVAL CODE "${_call})")
  endfunction()
endif()

include(generated/rexglue.cmake)

# The OS layer (src/platform/platform.h): one set of files per OS, shared by
# the executable, the mantra plugin and mantra_shadergen.
if(WIN32)
    set(ASURA_PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/platform/platform_win.cpp)
elseif(ANDROID)
    set(ASURA_PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/platform/platform_posix.cpp
                               ${CMAKE_SOURCE_DIR}/src/platform/platform_android.cpp)
else()
    set(ASURA_PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/platform/platform_posix.cpp
                               ${CMAKE_SOURCE_DIR}/src/platform/platform_linux.cpp)
endif()

# Sources
set(ASURAWRATH_SOURCES
    src/main.cpp
    src/game/frame_rate.cpp
    src/game/skip_intro.cpp
    src/game/game_files.cpp
    src/game/graphics_effects.cpp
    src/game/translation.cpp
    src/platform/gpu_driver.cpp
    src/input/controller_mappings.cpp
    src/input/pad_remap.cpp
    ${ASURA_PLATFORM_SOURCES}
    src/ui/about_info.cpp
    src/ui/game_assets.cpp
    src/ui/game_font.cpp
    src/ui/fps_counter.cpp
    src/ui/game_toast.cpp
    src/ui/controller_setup.cpp
    src/ui/launcher.cpp
    src/ui/launcher_text.cpp
    src/ui/notice_toast.cpp
    src/ui/save_data.cpp
    src/ui/translation.cpp
    src/ui/window_icon.cpp
    $<$<PLATFORM_ID:Windows>:src/res/asura_wrath.rc>
)

if(WIN32)
    add_executable(asura_wrath WIN32 ${ASURAWRATH_SOURCES})
elseif(ANDROID)
    # Android: libasura_wrath.so, loaded by the app's activity (android/),
    # which calls its SDL_main.
    add_library(asura_wrath SHARED ${ASURAWRATH_SOURCES})
    # The NDK's libc++ keeps std::jthread behind its experimental flag.
    target_compile_options(asura_wrath PRIVATE -fexperimental-library)
    # Custom Vulkan drivers (Turnip, newer Qualcomm ones) for Snapdragon
    # phones: libadrenotools (BSD-2-Clause), arm64 only. Its hook libraries
    # (libhook_impl.so, libmain_hook.so, ...) go in the APK next to ours; on
    # other ABIs empty stand-ins of the same names keep gradle's target list
    # the same (android/app/build.gradle.kts).
    if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        include(FetchContent)
        FetchContent_Declare(adrenotools
            GIT_REPOSITORY https://github.com/bylaws/libadrenotools.git
            GIT_TAG 8fae8ce254dfc1344527e05301e43f37dea2df80
            GIT_SUBMODULES_RECURSE TRUE)
        FetchContent_MakeAvailable(adrenotools)
        target_link_libraries(asura_wrath PRIVATE adrenotools)
        target_compile_definitions(asura_wrath PRIVATE ASURA_ADRENOTOOLS)
    else()
        foreach(_hook hook_impl main_hook file_redirect_hook gsl_alloc_hook)
            add_library(${_hook} SHARED "${CMAKE_SOURCE_DIR}/src/platform/empty.cpp")
        endforeach()
    endif()
else()
    add_executable(asura_wrath ${ASURAWRATH_SOURCES})
endif()

# Settings > About shows the git commit it was built from: build_info.h is
# regenerated on every build (rewritten only when the commit or the
# modified state changes, which recompiles about_info.cpp alone).
set(ASURA_BUILD_INFO_DIR "${CMAKE_BINARY_DIR}/generated_build_info")
add_custom_target(asura_build_info
  COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
          "-DOUT=${ASURA_BUILD_INFO_DIR}/build_info.h"
          -P "${CMAKE_SOURCE_DIR}/tools/build_info.cmake"
  BYPRODUCTS "${ASURA_BUILD_INFO_DIR}/build_info.h"
  VERBATIM)
add_dependencies(asura_wrath asura_build_info)
set_source_files_properties(src/ui/about_info.cpp PROPERTIES
  INCLUDE_DIRECTORIES "${ASURA_BUILD_INFO_DIR}")

# The launcher lists GPUs through the SDK's VulkanInstance, whose header
# includes renderdoc_app.h.
target_include_directories(asura_wrath PRIVATE ${REXSDK_DIR}/thirdparty/renderdoc)
# The launcher extracts the game's UI fonts from its .xxx packages with
# mantra_shadergen's package reader (LZX through libmspack).
target_sources(asura_wrath PRIVATE "${CMAKE_SOURCE_DIR}/tools/mantra_shadergen/xxx_package.cpp")
target_include_directories(asura_wrath PRIVATE "${CMAKE_SOURCE_DIR}/tools/mantra_shadergen")
target_link_libraries(asura_wrath PRIVATE mspack)

# mantra: native-renderer GPU plugin, built from this repo (src/mantra_renderer).
add_subdirectory(src/mantra_renderer)

# mantra_shadergen: offline shader translation (the launcher's shader step).
option(ASURA_BUILD_SHADERGEN "Build the mantra_shadergen tool" ON)
if(ASURA_BUILD_SHADERGEN AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32))
  add_subdirectory(tools/mantra_shadergen)
  add_subdirectory(tools/xxx_unpack)  # .xxx -> UE3 package bodies (asset extraction)
elseif(ASURA_BUILD_SHADERGEN AND ANDROID)
  add_subdirectory(tools/mantra_shadergen)  # the launcher's on-phone build
endif()

rexglue_setup_target(asura_wrath GPU_PLUGINS xenos mantra)

# The SDK writes librexruntime.so (rexruntime.dll) into its own out/ dir;
# stage it next to the executable so the $ORIGIN RUNPATH, the Windows DLL
# search and the GPU plugins find it.
if(WIN32 OR (UNIX AND NOT APPLE))
  add_custom_command(TARGET asura_wrath POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:rexruntime> $<TARGET_FILE_DIR:asura_wrath>
      VERBATIM)
endif()
if(WIN32)
  # SHGetKnownFolderPath, ShellExecuteW, CommandLineToArgvW (platform_win.cpp).
  target_link_libraries(asura_wrath PRIVATE shell32 ole32)
endif()

# The project's own UI art (assets/), staged next to the executable.
add_custom_command(TARGET asura_wrath POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:asura_wrath>/assets
    VERBATIM)
