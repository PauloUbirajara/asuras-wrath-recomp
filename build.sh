#!/usr/bin/env bash
# Build Asura's Wrath Recompiled on Linux.
#
#   ./build.sh                  release build for your CPU (-march=native)
#   ./build.sh --debug          debug build
#   ./build.sh --portable       -msse4.1 baseline, runs on any SSE4.1 CPU (for sharing)
#   ./build.sh --codegen        force a full rexglue codegen re-run
#   ./build.sh --clean          wipe the build directory first
#   ./build.sh --skip-cpu-check skip the check of your CPU (building for another PC)
#   ./build.sh --windows        build for Windows x64 from Linux (clang + lld-link with
#                               Microsoft's SDK from tools/setup_windows_cross.sh;
#                               needs the Linux build first, for its codegen tool)
#   ./build.sh --android        the Android APKs (one per ABI plus a universal one;
#                               tools/package.sh --android; ASURA_ABIS=arm64-v8a for one;
#                               needs the Android SDK in $ANDROID_HOME or ~/Android/Sdk;
#                               the first one also cross-builds DXC, ~8 min per ABI:
#                               tools/build_dxc_android.sh)
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
WINDOWS=false
ANDROID=false
for arg in "$@"; do
  case "$arg" in
    --debug) CONFIG=debug ;;
    --relwithdebinfo) CONFIG=relwithdebinfo ;;
    --release) CONFIG=release ;;
    --portable) PORTABLE=true ;;
    --codegen) CODEGEN=true ;;
    --clean) CLEAN=true ;;
    --skip-cpu-check) CPU_CHECK=false ;;
    --windows) WINDOWS=true ;;
    --android) ANDROID=true ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "[!] Unknown argument: $arg" >&2; exit 1 ;;
  esac
done

# Android: Gradle drives the NDK build of the same CMake project (android/).
if $ANDROID; then
  export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
  if [ ! -d "$ANDROID_HOME/ndk" ]; then
    echo "[!] No Android SDK/NDK in $ANDROID_HOME (see android/README.md)" >&2
    exit 1
  fi
  [ -f android/local.properties ] ||
    printf 'sdk.dir=%s\ncmake.dir=%s\n' "$ANDROID_HOME" "$(dirname "$(dirname "$(command -v cmake)")")" \
      > android/local.properties
  # DXC for the on-phone shader build (mantra_shadergen in the APK).
  tools/build_dxc_android.sh $(echo "${ASURA_ABIS:-arm64-v8a,x86_64}" | tr ',' ' ')
  task=assembleRelease
  [ "$CONFIG" = debug ] && task=assembleDebug
  (cd android && ./gradlew ${ASURA_ABIS:+-Pasura.abis=$ASURA_ABIS} "$task")
  kind=release; [ "$task" = assembleDebug ] && kind=debug || true
  for apk in android/app/build/outputs/apk/$kind/app-*-$kind.apk; do
    echo "[+] APK: $apk ($(du -h "$apk" | cut -f1))"
  done
  echo "[+] tools/package.sh --android copies them to out/package/ with checksums"
  exit 0
fi

PRESET="linux-amd64-$CONFIG"
if $WINDOWS; then
  PRESET="win-amd64-$CONFIG-cross"
  if [ ! -d out/toolchain/xwin/sdk ]; then
    echo "[!] No Windows SDK yet: run tools/setup_windows_cross.sh first" >&2
    exit 1
  fi
  if [ ! -x thirdparty/rexglue/out/linux-amd64/rexglue ]; then
    echo "[!] The Windows build runs the Linux codegen tool: run ./build.sh first" >&2
    exit 1
  fi
fi
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

if $WINDOWS; then
  echo "[+] Built: $BUILD_DIR/asura_wrath.exe"
else
  echo "[+] Built: $BUILD_DIR/asura_wrath"
fi
