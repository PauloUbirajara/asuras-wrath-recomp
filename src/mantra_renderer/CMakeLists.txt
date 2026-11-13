# rexgpu-mantra: native-renderer GPU plugin, built from this repo.
# Compiles the SDK's backend-independent GPU base sources by path so
# thirdparty/rexglue stays unmodified.
set(MANTRA_SDK_GFX "${REXSDK_DIR}/src/graphics")

set(MANTRA_BASE_SOURCES
    ${MANTRA_SDK_GFX}/flags.cpp
    ${MANTRA_SDK_GFX}/register_file.cpp
    ${MANTRA_SDK_GFX}/registers.cpp
    ${MANTRA_SDK_GFX}/pipeline/shader/shader.cpp
    ${MANTRA_SDK_GFX}/format/ucode.cpp
    ${MANTRA_SDK_GFX}/sampler_info.cpp
    ${MANTRA_SDK_GFX}/graphics_system.cpp
    ${MANTRA_SDK_GFX}/command_processor.cpp
    ${MANTRA_SDK_GFX}/util/draw_extent_estimator.cpp
    ${MANTRA_SDK_GFX}/util/draw.cpp
    ${MANTRA_SDK_GFX}/packet_disassembler.cpp
    ${MANTRA_SDK_GFX}/primitive_processor.cpp
    ${MANTRA_SDK_GFX}/pipeline/render_target/cache.cpp
    ${MANTRA_SDK_GFX}/shared_memory.cpp
    ${MANTRA_SDK_GFX}/pipeline/texture/cache.cpp
    ${MANTRA_SDK_GFX}/pipeline/shader/interpreter.cpp
    ${MANTRA_SDK_GFX}/pipeline/shader/translator.cpp
    ${MANTRA_SDK_GFX}/pipeline/shader/translator_disasm.cpp
)

# Host shaders (GLSL -> SPIR-V words, #included by copy_pipelines.cpp).
find_program(MANTRA_GLSLC glslc REQUIRED)
set(MANTRA_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(MANTRA_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${MANTRA_SHADER_OUT}")
set(MANTRA_SHADER_INCS)
# <output name>|<source>|<extra glslc args>
foreach(_spec
    "fullscreen.vert|fullscreen.vert|"
    "copy.frag|copy.frag|"
    "copy_ms.frag|copy.frag|-DMULTISAMPLE"
    "rect_list.geom|rect_list.geom|")
  string(REPLACE "|" ";" _parts "${_spec}")
  list(GET _parts 0 _name)
  list(GET _parts 1 _src)
  list(GET _parts 2 _args)
  set(_out "${MANTRA_SHADER_OUT}/${_name}.inc")
  add_custom_command(
      OUTPUT "${_out}"
      COMMAND ${MANTRA_GLSLC} --target-env=vulkan1.1 -O -mfmt=num ${_args}
              "${MANTRA_SHADER_DIR}/${_src}" -o "${_out}"
      DEPENDS "${MANTRA_SHADER_DIR}/${_src}"
      COMMENT "glslc ${_name}"
      VERBATIM)
  list(APPEND MANTRA_SHADER_INCS "${_out}")
endforeach()

add_library(rexgpu-mantra SHARED
    plugin_main.cpp
    native_graphics_system.cpp
    native_command_processor.cpp
    shader_cache.cpp
    vk_image.cpp
    copy_pipelines.cpp
    render_targets.cpp
    shader_interface.cpp
    solid_fill.cpp
    buffer_cache.cpp
    draw_pipelines.cpp
    guest_draw.cpp
    texture_cache.cpp
    ${MANTRA_SHADER_INCS}
    ${MANTRA_BASE_SOURCES}
)

set_target_properties(rexgpu-mantra PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
if(UNIX AND NOT APPLE)
  set_target_properties(rexgpu-mantra PROPERTIES
      BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN" BUILD_WITH_INSTALL_RPATH ON)
endif()

target_include_directories(rexgpu-mantra PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MANTRA_SHADER_OUT}
    ${REXSDK_DIR}
    ${REXSDK_DIR}/include
    ${REXSDK_DIR}/thirdparty/renderdoc
)

# Plugin-local: guest-visible GPU math should not go through fast-math.
target_compile_options(rexgpu-mantra PRIVATE -fno-fast-math -Wno-unused-variable)

if(COMMAND rexglue_apply_target_settings)
  rexglue_apply_target_settings(rexgpu-mantra)   # same settings the app targets get
endif()

if(COMMAND rex_target_pch)
  rex_target_pch(rexgpu-mantra
      <mutex> <unordered_map>
      <rex/math.h> <rex/memory.h> <rex/thread/mutex.h> <rex/system/xmemory.h>
      <rex/graphics/xenos.h> <rex/graphics/registers.h>
      <rex/graphics/format/ucode.h> <rex/graphics/pipeline/shader/shader.h>)
endif()

# Everything PRIVATE. Link only rexruntime, never the rexcore/rexui object libs.
target_link_libraries(rexgpu-mantra PRIVATE rexruntime)
foreach(_t xxHash::xxhash Vulkan::Headers GPUOpen::VulkanMemoryAllocator)
  if(TARGET ${_t})
    target_link_libraries(rexgpu-mantra PRIVATE ${_t})
  endif()
endforeach()

# rexglue_setup_target stages GPU plugins only when the executable relinks;
# re-stage this one whenever it is rebuilt on its own.
if(TARGET asura_wrath)
  add_custom_command(TARGET rexgpu-mantra POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:rexgpu-mantra> $<TARGET_FILE_DIR:asura_wrath>
      VERBATIM)
endif()
