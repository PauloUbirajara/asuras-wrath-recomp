#!/usr/bin/env bash
# Cross-builds DXC (libdxcompiler.so) for Android, for the launcher's on-phone
# shader build: mantra_shadergen links it, and the APK packages it.
#
#   tools/build_dxc_android.sh [abi...]      default: arm64-v8a x86_64
#
# The same DXC version as XenosRecomp's prebuilt dxc-bin (v1.8.2407), so a
# cache built on the phone matches one built on a computer. Everything goes
# in out/dxc/: src/ (the clone), host/ (llvm-tblgen and clang-tblgen, which
# run during the build), android-<abi>/ (the build), jniLibs/<abi>/ (the
# stripped library, where CMake and gradle take it from). An ABI whose
# library is already there is skipped; delete jniLibs/<abi> to rebuild it.
# About 8 minutes per ABI on 12 cores; ~2 GB per build folder.
set -euo pipefail
cd "$(dirname "$0")/.."

DXC_TAG=v1.8.2407
ROOT="$PWD/out/dxc"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
# The NDK the app builds with (android/app/build.gradle.kts).
NDK_VERSION=$(sed -n 's/.*ndkVersion = "\(.*\)".*/\1/p' android/app/build.gradle.kts)
NDK="$ANDROID_HOME/ndk/$NDK_VERSION"
MIN_SDK=$(sed -n 's/.*minSdk = \([0-9]*\).*/\1/p' android/app/build.gradle.kts)
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
[ -x "$STRIP" ] || { echo "[!] No NDK $NDK_VERSION in $ANDROID_HOME/ndk" >&2; exit 1; }

abis=("$@")
[ ${#abis[@]} -gt 0 ] || abis=(arm64-v8a x86_64)

common=(-G Ninja -C "$ROOT/src/cmake/caches/PredefinedParams.cmake"
        -DCMAKE_BUILD_TYPE=Release -DHLSL_INCLUDE_TESTS=OFF -DSPIRV_BUILD_TESTS=OFF
        -DLLVM_APPEND_VC_REV=OFF)

need_build=false
for abi in "${abis[@]}"; do
  [ -f "$ROOT/jniLibs/$abi/libdxcompiler.so" ] || need_build=true
done
$need_build || { echo "[+] DXC for Android: already built (${abis[*]})"; exit 0; }

if [ ! -f "$ROOT/src/CMakeLists.txt" ]; then
  echo "[*] Fetching DXC $DXC_TAG"
  mkdir -p "$ROOT"
  git clone -q --depth 1 --branch "$DXC_TAG" --recurse-submodules --shallow-submodules \
    https://github.com/microsoft/DirectXShaderCompiler.git "$ROOT/src"
fi

if [ ! -x "$ROOT/host/bin/clang-tblgen" ]; then
  echo "[*] DXC: host table generators"
  cmake -S "$ROOT/src" -B "$ROOT/host" "${common[@]}" \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > "$ROOT/host-configure.log"
  ninja -C "$ROOT/host" llvm-tblgen clang-tblgen > "$ROOT/host-build.log"
fi

for abi in "${abis[@]}"; do
  out="$ROOT/jniLibs/$abi/libdxcompiler.so"
  [ -f "$out" ] && continue
  case "$abi" in
    arm64-v8a) triple=aarch64-linux-android ;;
    x86_64) triple=x86_64-linux-android ;;
    *) echo "[!] Unsupported ABI $abi" >&2; exit 1 ;;
  esac
  echo "[*] DXC: $abi (several minutes; log: out/dxc/android-$abi-build.log)"
  cmake -S "$ROOT/src" -B "$ROOT/android-$abi" "${common[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$MIN_SDK" -DANDROID_STL=c++_shared \
    -DLLVM_TABLEGEN="$ROOT/host/bin/llvm-tblgen" -DCLANG_TABLEGEN="$ROOT/host/bin/clang-tblgen" \
    -DLLVM_HOST_TRIPLE="$triple" > "$ROOT/android-$abi-configure.log"
  ninja -C "$ROOT/android-$abi" dxcompiler > "$ROOT/android-$abi-build.log"
  mkdir -p "$(dirname "$out")"
  "$STRIP" -o "$out" "$ROOT/android-$abi/lib/libdxcompiler.so"
  echo "[+] $out ($(du -h "$out" | cut -f1))"
done
