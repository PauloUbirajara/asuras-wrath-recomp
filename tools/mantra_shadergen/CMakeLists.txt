# mantra_shadergen: offline shader translation for the mantra renderer.
# Builds XenosRecomp's recompiler (thirdparty/XenosRecomp, unmodified) by path,
# using rexglue's fmt/xxHash/libmspack and XenosRecomp's prebuilt DXC.
set(XENOS_RECOMP_DIR "${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp")
set(XENOS_RECOMP_SRC "${XENOS_RECOMP_DIR}/XenosRecomp")
set(DXC_BIN_DIR "${XENOS_RECOMP_DIR}/thirdparty/dxc-bin")

if(NOT EXISTS "${XENOS_RECOMP_SRC}/shader_recompiler.cpp" OR NOT EXISTS "${DXC_BIN_DIR}/CMakeLists.txt")
  message(WARNING "mantra_shadergen: XenosRecomp or its dxc-bin submodule is missing; "
                  "run 'git submodule update --init thirdparty/XenosRecomp' and "
                  "'git -C thirdparty/XenosRecomp submodule update --init thirdparty/dxc-bin'")
  return()
endif()

if(NOT TARGET Microsoft::DirectXShaderCompiler)
  add_subdirectory("${DXC_BIN_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dxc-bin")
endif()

# mantra_shader_common.hlsl (our descriptor-based replacement for XenosRecomp's
# shader_common.h) is prepended to every generated shader; embed it so the tool
# does not depend on the source tree at run time.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/mantra_shader_common.hlsl" MANTRA_SHADER_COMMON)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/shader_common_hlsl.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/shader_common_hlsl.h" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/mantra_shader_common.hlsl")

add_executable(mantra_shadergen
    main.cpp
    translator.cpp
    xxx_package.cpp
    "${XENOS_RECOMP_SRC}/shader_recompiler.cpp"
    "${XENOS_RECOMP_SRC}/dxc_compiler.cpp"
    # Workers are this tool run again (platform::Process).
    ${ASURA_PLATFORM_SOURCES}
)

# XenosRecomp sources expect pch.h to be included first; DXC's WinAdapter.h
# needs -fms-extensions (translator.cpp includes it through pch.h too).
set_source_files_properties(
    "${XENOS_RECOMP_SRC}/shader_recompiler.cpp"
    "${XENOS_RECOMP_SRC}/dxc_compiler.cpp"
    PROPERTIES COMPILE_OPTIONS "-include;pch.h;-Wno-switch;-Wno-unused-variable;-Wno-null-arithmetic;-fms-extensions;-w")
set_source_files_properties(translator.cpp PROPERTIES COMPILE_OPTIONS "-fms-extensions;-w")

target_include_directories(mantra_shadergen PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"            # stub smolv.h / zstd.h
    "${XENOS_RECOMP_SRC}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_SOURCE_DIR}/src"                       # mantra_renderer/shader_key.h
    $<TARGET_PROPERTY:rexruntime,INTERFACE_INCLUDE_DIRECTORIES>
)
target_compile_definitions(mantra_shadergen PRIVATE
    $<TARGET_PROPERTY:rexruntime,INTERFACE_COMPILE_DEFINITIONS>)

target_link_libraries(mantra_shadergen PRIVATE
    Microsoft::DirectXShaderCompiler
    fmt::fmt
    xxHash::xxhash
    mspack
)

# Run from the build directory: find the DXC library next to the tool
# (libdxcompiler.so; on Windows dxcompiler.dll and dxil.dll, which signs the
# DXIL DXC also builds).
if(WIN32)
  # dxcapi.h needs IUnknown and IStream, which WIN32_LEAN_AND_MEAN (from
  # rexruntime) leaves out of windows.h.
  target_compile_options(mantra_shadergen PRIVATE
      "SHELL:-include windows.h" "SHELL:-include objidl.h")
  target_link_libraries(mantra_shadergen PRIVATE shell32 ole32)
  add_custom_command(TARGET mantra_shadergen POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:Microsoft::DirectXShaderCompiler> $<TARGET_FILE:Microsoft::DXIL>
          $<TARGET_FILE_DIR:mantra_shadergen>
      VERBATIM)
else()
  add_custom_command(TARGET mantra_shadergen POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:Microsoft::DirectXShaderCompiler> $<TARGET_FILE_DIR:mantra_shadergen>
      VERBATIM)
endif()
set_target_properties(mantra_shadergen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    BUILD_RPATH "$ORIGIN")
