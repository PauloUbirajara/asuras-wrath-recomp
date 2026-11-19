# Toolchain: build for Windows x64 on Linux, with clang targeting the MSVC
# ABI (what the SDK's Windows code and DXC's import library expect; MinGW
# can't build the SDK's __try/__except) and lld-link.
#
# The Microsoft C runtime and Windows SDK come from xwin (not redistributable,
# so not in the repo); tools/setup_windows_cross.sh downloads them to
# out/toolchain/xwin. Use the preset: cmake --preset win-amd64-release-cross
#
#   XWIN_DIR       the xwin "splat" folder (default <repo>/out/toolchain/xwin)
#   LLVM_SUFFIX    clang/lld name suffix (default -20: clang-20, lld-link)
#   REXGLUE_HOST_TOOL  the Linux rexglue codegen tool (CMakeLists.txt)
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT XWIN_DIR)
  set(XWIN_DIR "${_repo}/out/toolchain/xwin")
endif()
if(NOT DEFINED LLVM_SUFFIX)
  set(LLVM_SUFFIX "-20")
endif()
if(NOT EXISTS "${XWIN_DIR}/crt/include" OR NOT EXISTS "${XWIN_DIR}/sdk/include/um")
  message(FATAL_ERROR "No Windows SDK in ${XWIN_DIR}: run tools/setup_windows_cross.sh")
endif()
# try_compile projects get a fresh cache: pass these through.
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR LLVM_SUFFIX)

find_program(_clang NAMES clang${LLVM_SUFFIX} clang REQUIRED)
find_program(_clangxx NAMES clang++${LLVM_SUFFIX} clang++ REQUIRED)
find_program(_rc NAMES llvm-rc${LLVM_SUFFIX} llvm-rc REQUIRED)
find_program(_mt NAMES llvm-mt${LLVM_SUFFIX} llvm-mt)
find_program(_ar NAMES llvm-ar${LLVM_SUFFIX} llvm-ar REQUIRED)  # clang++ driver: GNU-style archives
find_program(_lld NAMES lld-link${LLVM_SUFFIX} lld-link REQUIRED)

set(CMAKE_C_COMPILER "${_clang}")
set(CMAKE_CXX_COMPILER "${_clangxx}")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_RC_COMPILER "${_rc}")
set(CMAKE_AR "${_ar}")
set(CMAKE_LINKER "${_lld}")
if(_mt)
  set(CMAKE_MT "${_mt}")
endif()

# Headers: the MSVC C/C++ runtime and the Windows SDK, searched after clang's
# own (its emmintrin.h etc. must win over MSVC's, which only declare the
# intrinsics). Added to the flags here rather than as *_INIT, so that
# -DCMAKE_CXX_FLAGS=... (build.sh's CPU flags) can't drop them.
set(_includes
    "${XWIN_DIR}/crt/include"
    "${XWIN_DIR}/sdk/include/ucrt"
    "${XWIN_DIR}/sdk/include/um"
    "${XWIN_DIR}/sdk/include/shared"
    "${XWIN_DIR}/sdk/include/winrt")
set(_flags "")
foreach(_dir IN LISTS _includes)
  string(APPEND _flags " -Xclang -internal-isystem -Xclang \"${_dir}\"")
endforeach()
foreach(_lang C CXX)
  if(NOT CMAKE_${_lang}_FLAGS MATCHES "internal-isystem")  # read more than once
    set(CMAKE_${_lang}_FLAGS "${CMAKE_${_lang}_FLAGS}${_flags}")
  endif()
endforeach()
set(_rc_flags "")
foreach(_dir IN LISTS _includes)
  string(APPEND _rc_flags " -I \"${_dir}\"")
endforeach()
set(CMAKE_RC_FLAGS_INIT "${_rc_flags}")

# Libraries (CMake links through lld-link itself). xwin leaves out the debug
# runtime (msvcrtd.lib): the compiler checks link as Release, and Debug builds
# need tools/setup_windows_cross.sh --debug-libs.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(_link "")
foreach(_dir crt/lib/x86_64 sdk/lib/um/x86_64 sdk/lib/ucrt/x86_64)
  string(APPEND _link " -L\"${XWIN_DIR}/${_dir}\"")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_link}")

# Look for libraries and headers in the SDK only; programs on the host.
set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
