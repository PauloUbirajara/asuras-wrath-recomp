#!/usr/bin/env bash
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REXSDK_DIR="${REXSDK_DIR:-$REPO_ROOT/tools/rexglue}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
REXGLUE_CXX_FLAGS="${REXGLUE_CXX_FLAGS:--mssse3}"

echo "[+] =========================================="
echo "[+] Building Asura's Wrath Recompiled (Linux)"
echo "[+] =========================================="

# 1. Build ReXGlue Engine SDK if missing
if [ ! -f "$REXSDK_DIR/out/linux-amd64/librexruntime.so" ] && [ ! -f "$REXSDK_DIR/out/linux-amd64/librexgsl.so" ] && [ ! -f "$REXSDK_DIR/out/linux-amd64/librexglue.so" ]; then
    echo "[+] Building ReXGlue engine SDK..."
    git submodule update --init --recursive
    mkdir -p "$REXSDK_DIR/build"
    CC=clang CXX=clang++ cmake -B "$REXSDK_DIR/build" -S "$REXSDK_DIR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_CXX_FLAGS="$REXGLUE_CXX_FLAGS"
    cmake --build "$REXSDK_DIR/build" --target rexglue --parallel "$(nproc)"
fi

# 2. Build asura_wrath_recomp Executable
echo "[+] Building asura_wrath_recomp executable..."
mkdir -p "$REPO_ROOT/build"
CC=clang CXX=clang++ cmake -B "$REPO_ROOT/build" -S "$REPO_ROOT" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DREXSDK_DIR="$REXSDK_DIR"

cmake --build "$REPO_ROOT/build" --parallel "$(nproc)"

rm -f "$REPO_ROOT/build/asurawrath"

echo "[+] Build completed successfully: $REPO_ROOT/build/asura_wrath_recomp"
