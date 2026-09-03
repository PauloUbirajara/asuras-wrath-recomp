#!/usr/bin/env bash
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PACKAGE=false
for arg in "$@"; do
    if [ "$arg" = "--package" ] || [ "$arg" = "--zip" ] || [ "$arg" = "-p" ]; then
        PACKAGE=true
    fi
done

echo "[+] =========================================="
echo "[+] Building Asura's Wrath Recompiled (Linux)"
echo "[+] =========================================="

git submodule update --init --recursive

cmake --preset linux-amd64-release
cmake --build "$REPO_ROOT/out/build/linux-amd64-release" --parallel "$(nproc)"

echo "[+] Build completed successfully: $REPO_ROOT/out/build/linux-amd64-release/asura_wrath_recomp"

if [ "$PACKAGE" = true ]; then
    echo "[+] Packaging release bundle..."
    DIST_DIR="$REPO_ROOT/out/dist/asura_wrath_recomp_linux"
    ZIP_PATH="$REPO_ROOT/out/dist/asura_wrath_recomp_linux.zip"
    rm -rf "$DIST_DIR" "$ZIP_PATH"
    mkdir -p "$DIST_DIR"

    cp "$REPO_ROOT/out/build/linux-amd64-release/asura_wrath_recomp" "$DIST_DIR/"
    cp "$REPO_ROOT/tools/rexglue/out/linux-amd64/"*.so "$DIST_DIR/" 2>/dev/null || true

    (cd "$REPO_ROOT/out/dist" && zip -r "asura_wrath_recomp_linux.zip" "asura_wrath_recomp_linux")

    echo "[+] Release bundle created at: $ZIP_PATH"
fi