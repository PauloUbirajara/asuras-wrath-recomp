#!/usr/bin/env bash
# Packages a built game for players: a .tar.gz of the binaries, plus an
# AppImage when appimagetool is available.
#
#   tools/package.sh                 package out/build/linux-amd64-release
#   tools/package.sh --appimage      also build an AppImage, with appimagetool
#                                    from APPIMAGETOOL=<path>, PATH, or
#                                    ../appimagetool/appimagetool next to the repo
#                                    (APPIMAGE_RUNTIME=<file> for an offline
#                                    runtime; otherwise appimagetool fetches it)
#   tools/package.sh --build-dir D   package another build directory
#   tools/package.sh --no-strip      keep symbols (for profiling)
#
# Build with ./build.sh --portable first for a package that runs on other
# CPUs: the default build uses -march=native.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/out/build/linux-amd64-release"
APPIMAGE=false
STRIP=true
while [ $# -gt 0 ]; do
  case "$1" in
    --appimage) APPIMAGE=true ;;
    --no-strip) STRIP=false ;;
    --build-dir) BUILD_DIR="$(cd "$2" && pwd)"; shift ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "[!] Unknown argument: $1" >&2; exit 1 ;;
  esac
  shift
done

NAME=asuras-wrath-recomp
VERSION="$(git -C "$REPO_ROOT" describe --always --dirty 2>/dev/null || echo dev)"
OUT="$REPO_ROOT/out/package"
STAGE="$OUT/$NAME"

[ -x "$BUILD_DIR/asura_wrath" ] || { echo "[!] No build in $BUILD_DIR (run ./build.sh)" >&2; exit 1; }
if grep -q -- "-march=native" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
  echo "[!] $BUILD_DIR was built with -march=native: the package only runs on CPUs"
  echo "    like this one. Use ./build.sh --portable for a package to share."
fi

# --- Stage ---------------------------------------------------------------------
rm -rf "$STAGE"
mkdir -p "$STAGE"
FILES=(asura_wrath librexruntime.so librexgpu-mantra.so mantra_shadergen libdxcompiler.so)
for f in "${FILES[@]}"; do
  [ -e "$BUILD_DIR/$f" ] || { echo "[!] Missing $BUILD_DIR/$f" >&2; exit 1; }
  cp -L "$BUILD_DIR/$f" "$STAGE/"
done
# The SDK's own renderer, for --gpu_plugin=xenos (optional).
[ -e "$BUILD_DIR/librexgpu-xenos.so" ] && cp -L "$BUILD_DIR/librexgpu-xenos.so" "$STAGE/"
cp -r "$REPO_ROOT/assets" "$STAGE/assets"
cat > "$STAGE/README.txt" <<'TXT'
Asura's Wrath Recompiled (Linux x86-64)

You need your own copy of the game, extracted:
    extract-xiso -x asura_wrath.iso -d extracted

Start ./asura_wrath (or the .AppImage). In the launcher:
  Settings > Game folder      the extracted folder (contains default.xex)
  Settings > Shader cache     Build, once (about 10 minutes)
  Play

Needs glibc 2.38+ (Ubuntu 24.04, Fedora 39, Arch or newer) and a Vulkan 1.3
driver (Mesa 23+ or NVIDIA 535+).
Files: settings, saves and logs in ~/.local/share/asura_wrath_recomp,
shader and pipeline caches in ~/.cache/asura_wrath_recomp.
Skip the launcher with --launcher=false (e.g. --launcher=false --game_data_root=<folder>).
TXT
# Libraries are found through RUNPATH $ORIGIN; drop build-tree entries.
if command -v patchelf >/dev/null; then
  patchelf --set-rpath '$ORIGIN' "$STAGE/mantra_shadergen"
fi
if $STRIP; then
  for f in "$STAGE"/asura_wrath "$STAGE"/mantra_shadergen "$STAGE"/*.so; do
    [ "$(basename "$f")" = libdxcompiler.so ] && continue  # prebuilt
    strip --strip-unneeded "$f"
  done
fi

# --- tar.gz --------------------------------------------------------------------
TARBALL="$OUT/$NAME-$VERSION-linux-x86_64.tar.gz"
tar -C "$OUT" -czf "$TARBALL" "$NAME"
echo "[+] $TARBALL ($(du -h "$TARBALL" | cut -f1))"

# --- AppImage ------------------------------------------------------------------
if $APPIMAGE; then
  TOOL="${APPIMAGETOOL:-$(command -v appimagetool || true)}"
  [ -z "$TOOL" ] && [ -f "$REPO_ROOT/../appimagetool/appimagetool" ] &&
    TOOL="$REPO_ROOT/../appimagetool/appimagetool"
  if [ -z "$TOOL" ]; then
    echo "[!] appimagetool not found: put it on PATH or set APPIMAGETOOL=<path>" >&2
    exit 1
  fi
  if [ ! -x "$TOOL" ]; then
    # Readable but not executable (e.g. owned by root): run a copy.
    mkdir -p "$REPO_ROOT/out/tools"
    cp "$TOOL" "$REPO_ROOT/out/tools/appimagetool"
    chmod +x "$REPO_ROOT/out/tools/appimagetool"
    TOOL="$REPO_ROOT/out/tools/appimagetool"
  fi
  # appimagetool is an AppImage itself: run it without FUSE.
  export APPIMAGE_EXTRACT_AND_RUN=1
  RUNTIME_ARGS=()
  [ -n "${APPIMAGE_RUNTIME:-}" ] && RUNTIME_ARGS=(--runtime-file "$APPIMAGE_RUNTIME")
  APPDIR="$OUT/AppDir"
  rm -rf "$APPDIR"
  mkdir -p "$APPDIR/usr/bin"
  cp -r "$STAGE"/. "$APPDIR/usr/bin/"
  cat > "$APPDIR/AppRun" <<'SH'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/usr/bin/asura_wrath" "$@"
SH
  chmod +x "$APPDIR/AppRun"
  cat > "$APPDIR/asura_wrath_recomp.desktop" <<'TXT'
[Desktop Entry]
Type=Application
Name=Asura's Wrath Recompiled
Comment=Asura's Wrath (Xbox 360), statically recompiled
Exec=asura_wrath
Icon=asura_wrath_recomp
Categories=Game;
Terminal=false
TXT
  # Placeholder icon (assets/ui/icon.png replaces it when present).
  if [ -f "$REPO_ROOT/assets/ui/icon.png" ]; then
    cp "$REPO_ROOT/assets/ui/icon.png" "$APPDIR/asura_wrath_recomp.png"
  else
    cat > "$APPDIR/asura_wrath_recomp.svg" <<'SVG'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
  <rect width="256" height="256" rx="36" fill="#1b1a1c"/>
  <path d="M128 34 L214 222 H176 L158 180 H98 L80 222 H42 Z M128 96 L108 148 H148 Z" fill="#e9e4dc"/>
  <rect x="40" y="200" width="176" height="10" fill="#cc5530"/>
</svg>
SVG
  fi
  APPIMAGE_FILE="$OUT/$NAME-$VERSION-x86_64.AppImage"
  ARCH=x86_64 "$TOOL" --no-appstream "${RUNTIME_ARGS[@]}" "$APPDIR" "$APPIMAGE_FILE"
  echo "[+] $APPIMAGE_FILE ($(du -h "$APPIMAGE_FILE" | cut -f1))"
fi
