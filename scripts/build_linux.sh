#!/usr/bin/env bash
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE="${BUILD_TYPE:-Release}"
CMAKE_ARGS=("-DCMAKE_BUILD_TYPE=$BUILD_TYPE")
if [ -n "$REXSDK_DIR" ]; then
    CMAKE_ARGS+=("-DREXSDK_DIR=$REXSDK_DIR")
fi

echo "[+] Building asura_wrath_recomp executable..."
mkdir -p "$REPO_ROOT/build"
CC=clang CXX=clang++ cmake -B "$REPO_ROOT/build" -S "$REPO_ROOT" "${CMAKE_ARGS[@]}"

cmake --build "$REPO_ROOT/build" --parallel "$(nproc)"

rm -f "$REPO_ROOT/build/asurawrath"

echo "[+] Build completed successfully: $REPO_ROOT/build/asura_wrath_recomp"
