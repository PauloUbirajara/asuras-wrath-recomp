#!/usr/bin/env bash
# Build Asura's Wrath Recompiled on Linux.
#
#   ./build.sh                  release build for your CPU (-march=native)
#   ./build.sh --debug          debug build
#   ./build.sh --portable       -msse4.1 baseline, runs on any SSE4.1 CPU (for sharing)
#   ./build.sh --codegen        force a full rexglue codegen re-run
#   ./build.sh --clean          wipe the build directory first
#   ./build.sh --skip-cpu-check skip the check of your CPU (building for another PC)
#
# CPU flags: the SDK core uses SSSE3/SSE4.1 intrinsics but only adds -msse4.1
# to app targets, so the flags are passed here for everything, SDK included.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

CONFIG=release
PORTABLE=false
CODEGEN=false
CLEAN=false
CPU_CHECK=true
for arg in "$@"; do
  case "$arg" in
    --debug) CONFIG=debug ;;
    --relwithdebinfo) CONFIG=relwithdebinfo ;;
    --release) CONFIG=release ;;
    --portable) PORTABLE=true ;;
    --codegen) CODEGEN=true ;;
    --clean) CLEAN=true ;;
    --skip-cpu-check) CPU_CHECK=false ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "[!] Unknown argument: $arg" >&2; exit 1 ;;
  esac
done

PRESET="linux-amd64-$CONFIG"
BUILD_DIR="$REPO_ROOT/out/build/$PRESET"
CXX="${CXX:-clang++-20}"

# --- CPU flags ---------------------------------------------------------------
cpu_has() { grep -m1 '^flags' /proc/cpuinfo | grep -qw "$1"; }

if ! $PORTABLE && echo | "$CXX" -march=native -x c++ -E - >/dev/null 2>&1; then
  CPU_FLAGS="-march=native"
else
  $PORTABLE || echo "[!] $CXX does not accept -march=native; using the portable baseline"
  CPU_FLAGS="-msse4.1"
  if $CPU_CHECK && [ -r /proc/cpuinfo ]; then
    missing=()
    for f in ssse3 sse4_1; do cpu_has "$f" || missing+=("$f"); done
    if [ ${#missing[@]} -gt 0 ]; then
      echo "[!] Your CPU lacks ${missing[*]}, required by the SDK (-msse4.1)." >&2
      echo "    Use --skip-cpu-check if the build is meant for another PC." >&2
      exit 1
    fi
  fi
fi
echo "[+] Preset: $PRESET, CPU flags: $CPU_FLAGS"

# --- Submodule ---------------------------------------------------------------
if [ ! -f thirdparty/rexglue/CMakeLists.txt ]; then
  git submodule update --init --recursive
fi

# --- Codegen -----------------------------------------------------------------
# The CMake build runs codegen itself (asura_wrath_codegen target) whenever its
# inputs change; --codegen drops the stamp to force a full re-run.
if $CODEGEN; then
  rm -f generated/default/codegen.build.stamp generated/default/codegen.stamp
fi

# --- Configure & build -------------------------------------------------------
$CLEAN && rm -rf "$BUILD_DIR"

# Flag changes need a fresh configure to reach the cache.
cmake --preset "$PRESET" \
  -DCMAKE_C_FLAGS="$CPU_FLAGS" \
  -DCMAKE_CXX_FLAGS="$CPU_FLAGS"
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

echo "[+] Built: $BUILD_DIR/asura_wrath"
