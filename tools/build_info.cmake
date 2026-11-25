# Generates build_info.h for Settings > About (src/ui/about_info.cpp).
#
# CMakeLists.txt runs this on every build:
#   cmake -DSOURCE_DIR=<repo> -DOUT=<build>/generated_build_info/build_info.h \
#         -P tools/build_info.cmake
# The header is rewritten only when its contents change, so a new commit or a
# dirty tree recompiles about_info.cpp alone.

find_package(Git QUIET)

set(ASURA_COMMIT "")
set(ASURA_BRANCH "")
set(ASURA_COMMIT_DATE "")
set(ASURA_DESCRIBE "")
set(ASURA_DIRTY 0)

if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --short HEAD
    OUTPUT_VARIABLE ASURA_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --abbrev-ref HEAD
    OUTPUT_VARIABLE ASURA_BRANCH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" show -s --format=%cs HEAD
    OUTPUT_VARIABLE ASURA_COMMIT_DATE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" describe --tags --always
    OUTPUT_VARIABLE ASURA_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  # Untracked files (game data, build outputs) don't make a build "dirty".
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}"
            status --porcelain --untracked-files=no --ignore-submodules
    OUTPUT_VARIABLE ASURA_CHANGED OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(ASURA_CHANGED)
    set(ASURA_DIRTY 1)
  endif()
endif()

# The values are C string literals.
foreach(var ASURA_COMMIT ASURA_BRANCH ASURA_COMMIT_DATE ASURA_DESCRIBE)
  string(REPLACE "\\" "\\\\" ${var} "${${var}}")
  string(REPLACE "\"" "\\\"" ${var} "${${var}}")
endforeach()

set(header "#pragma once
#define ASURA_GIT_COMMIT \"${ASURA_COMMIT}\"
#define ASURA_GIT_BRANCH \"${ASURA_BRANCH}\"
#define ASURA_GIT_COMMIT_DATE \"${ASURA_COMMIT_DATE}\"
#define ASURA_GIT_DESCRIBE \"${ASURA_DESCRIBE}\"
#define ASURA_GIT_DIRTY ${ASURA_DIRTY}
")

get_filename_component(dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${dir}")
set(tmp "${OUT}.in")
file(WRITE "${tmp}" "${header}")
# configure_file copies only when the contents differ (unlike file(WRITE)).
configure_file("${tmp}" "${OUT}" COPYONLY)
file(REMOVE "${tmp}")
