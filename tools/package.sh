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
#   tools/package.sh --version V     name the files with V (default: the git tag
#                                    on HEAD without its "v", else the commit)
#   tools/package.sh --clean         delete earlier packages in out/package first
#   tools/package.sh --windows       package the Windows build made on Linux
#                                    (./build.sh --windows) as a .zip instead
#   tools/package.sh --android       the APKs from ./build.sh --android instead
#                                    (universal, arm64-v8a, x86_64)
#   tools/package.sh --all           all of them: Linux .tar.gz + AppImage,
#                                    Windows .zip and the APKs (each from its
#                                    existing build; one missing is reported and
#                                    the others are still made)
#
# Writes out/package/asuras-wrath-recomp-<version>-linux-x86_64.tar.gz (and
# .AppImage) plus SHA256SUMS-linux (SHA256SUMS-windows, SHA256SUMS-android for the
# others); the staging folders are removed afterwards.
#
# The default build is tuned for your CPU (-march=native) and may crash on
# other CPUs: build with ./build.sh --portable first for a package to share.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/out/build/linux-amd64-release"
APPIMAGE=false
STRIP=true
CLEAN=false
WINDOWS=false
ANDROID=false
ALL=false
PASS=()  # options handed to each package of --all
BUILD_DIR_SET=false
VERSION=""
while [ $# -gt 0 ]; do
  case "$1" in
    --appimage) APPIMAGE=true ;;
    --no-strip) STRIP=false; PASS+=(--no-strip) ;;
    --build-dir) BUILD_DIR="$(cd "$2" && pwd)"; BUILD_DIR_SET=true; shift ;;
    --windows) WINDOWS=true ;;
    --android) ANDROID=true ;;
    --all) ALL=true ;;
    --version) VERSION="$2"; shift ;;
    --clean) CLEAN=true ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "[!] Unknown argument: $1" >&2; exit 1 ;;
  esac
  shift
done

NAME=asuras-wrath-recomp
if [ -z "$VERSION" ]; then
  VERSION="$(git -C "$REPO_ROOT" describe --tags --exact-match 2>/dev/null || true)"
  VERSION="${VERSION#v}"
  [ -n "$VERSION" ] || VERSION="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo dev)"
fi
if $ALL && { $WINDOWS || $ANDROID || $APPIMAGE || $BUILD_DIR_SET; }; then
  echo "[!] --all makes every package: drop --windows, --android, --appimage and --build-dir" >&2
  exit 1
fi
if [ -z "${ASURA_PACKAGE_ALL:-}" ] &&
  [ -n "$(git -C "$REPO_ROOT" status --porcelain --untracked-files=no --ignore-submodules 2>/dev/null)" ]; then
  echo "[!] The checkout has uncommitted changes: the package is not exactly $VERSION."
fi
OUT="$REPO_ROOT/out/package"
WORK="$OUT/.work"  # staging, removed at the end
STAGE="$WORK/$NAME"
BASE="$NAME-$VERSION-linux-x86_64"
$CLEAN && rm -rf "$OUT"
mkdir -p "$OUT"

# --- All (Linux, Windows, Android) ------------------------------------------------
# One run of this script per package, all with the same version.
if $ALL; then
  failed=()
  for kind in linux windows android; do
    echo "[+] --- $kind ---"
    case "$kind" in
      linux) args=(--appimage) ;;
      *) args=(--$kind) ;;
    esac
    ASURA_PACKAGE_ALL=1 "$0" --version "$VERSION" "${PASS[@]}" "${args[@]}" || failed+=("$kind")
  done
  if [ ${#failed[@]} -gt 0 ]; then
    echo "[!] Not packaged: ${failed[*]} (see above)" >&2
    exit 1
  fi
  echo "[+] All packages in $OUT"
  exit 0
fi

trap 'rm -rf "$WORK"' EXIT

# --- Android (.apk) ---------------------------------------------------------------
# Gradle already packs and signs them: copied under the package names.
if $ANDROID; then
  APK_DIR="$REPO_ROOT/android/app/build/outputs/apk/release"
  apks=("$APK_DIR"/app-*-release.apk)
  [ -e "${apks[0]}" ] || { echo "[!] No APKs in $APK_DIR (run ./build.sh --android)" >&2; exit 1; }
  names=()
  for apk in "${apks[@]}"; do
    abi=${apk##*/app-}; abi=${abi%-release.apk}
    dest="$NAME-$VERSION-android-$abi.apk"
    cp "$apk" "$OUT/$dest"
    names+=("$dest")
    echo "[+] $OUT/$dest ($(du -h "$OUT/$dest" | cut -f1))"
  done
  (cd "$OUT" && sha256sum "${names[@]}" > "SHA256SUMS-android")
  echo "[+] $OUT/SHA256SUMS-android"
  exit 0
fi

# --- Windows (.zip) ---------------------------------------------------------------
if $WINDOWS; then
  $BUILD_DIR_SET || BUILD_DIR="$REPO_ROOT/out/build/win-amd64-release-cross"
  BASE="$NAME-$VERSION-windows-x86_64"
  [ -f "$BUILD_DIR/asura_wrath.exe" ] || { echo "[!] No build in $BUILD_DIR (run ./build.sh --windows)" >&2; exit 1; }
  if grep -q -- "-march=native" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
    echo "[!] $BUILD_DIR was built for your CPU (-march=native): the package may not"
    echo "    run on other CPUs. Use ./build.sh --windows --portable for a package to share."
  fi
  rm -rf "$STAGE"
  mkdir -p "$STAGE"
  for f in asura_wrath.exe rexruntime.dll rexgpu-mantra.dll mantra_shadergen.exe dxcompiler.dll dxil.dll; do
    [ -e "$BUILD_DIR/$f" ] || { echo "[!] Missing $BUILD_DIR/$f" >&2; exit 1; }
    cp -L "$BUILD_DIR/$f" "$STAGE/"
  done
  # Any other DLL staged next to the executable (the SDK's xenos renderer, SDL3).
  for f in "$BUILD_DIR"/*.dll; do
    [ -e "$STAGE/$(basename "$f")" ] || cp -L "$f" "$STAGE/"
  done
  cp -r "$REPO_ROOT/assets" "$STAGE/assets"
  sed 's/$/\r/' > "$STAGE/README.txt" <<'TXT'
Asura's Wrath Recompiled (Windows x86-64)

You need your own copy of the game, extracted (or drop the .iso on the
launcher window to extract it).

Start asura_wrath.exe. In the launcher:
  Game data > Folders  the game folder (the extracted folder, with default.xex),
                       or drop the .iso on the window to extract it
  Game data > Shaders  Build, once (about 10 minutes)
  Play

Needs Windows 10 or 11 (x64) and a Vulkan 1.3 driver (current AMD, NVIDIA or
Intel drivers).
If it says MSVCP140.dll or VCRUNTIME140.dll is missing, install the Microsoft
Visual C++ Redistributable (x64): https://aka.ms/vs/17/release/vc_redist.x64.exe
Files: everything the game makes (saves, shader and pipeline caches in cache\,
logs in logs\) goes in Documents\asura_wrath_recomp, or the folder chosen in
Game data > Folders > Files folder.
Skip the launcher with --launcher=false (e.g. --launcher=false --game_data_root=<folder>).
TXT
  ZIP="$OUT/$BASE.zip"
  rm -f "$ZIP"
  (cd "$WORK" && zip -qr9 "$ZIP" "$NAME")
  echo "[+] $ZIP ($(du -h "$ZIP" | cut -f1))"
  (cd "$OUT" && sha256sum "$BASE.zip" > "SHA256SUMS-windows")
  echo "[+] $OUT/SHA256SUMS-windows"
  exit 0
fi

[ -x "$BUILD_DIR/asura_wrath" ] || { echo "[!] No build in $BUILD_DIR (run ./build.sh)" >&2; exit 1; }
if grep -q -- "-march=native" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
  echo "[!] $BUILD_DIR was built for your CPU (-march=native): the package may not"
  echo "    run on other CPUs. Use ./build.sh --portable for a package to share."
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
  Game data > Folders  the game folder (the extracted folder, with default.xex),
                       or drop the .iso on the window to extract it
  Game data > Shaders  Build, once (about 10 minutes)
  Play

Needs glibc 2.38+ (Ubuntu 24.04, Fedora 39, Arch or newer) and a Vulkan 1.3
driver (Mesa 23+ or NVIDIA 535+).
Files: everything the game makes (saves, shader and pipeline caches in
cache/, logs in logs/) goes in ~/.local/share/asura_wrath_recomp, or the folder
chosen in Game data > Folders > Files folder.
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
TARBALL="$OUT/$BASE.tar.gz"
tar -C "$WORK" -czf "$TARBALL" "$NAME"
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
  APPDIR="$WORK/AppDir"
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
  APPIMAGE_FILE="$OUT/$BASE.AppImage"
  ARCH=x86_64 "$TOOL" --no-appstream "${RUNTIME_ARGS[@]}" "$APPDIR" "$APPIMAGE_FILE"
  echo "[+] $APPIMAGE_FILE ($(du -h "$APPIMAGE_FILE" | cut -f1))"
fi

# --- Checksums -----------------------------------------------------------------
(cd "$OUT" && sha256sum "$BASE".tar.gz $([ -f "$BASE.AppImage" ] && echo "$BASE.AppImage") > "SHA256SUMS-linux")
echo "[+] $OUT/SHA256SUMS-linux"
