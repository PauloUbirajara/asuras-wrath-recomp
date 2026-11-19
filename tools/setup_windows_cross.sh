#!/usr/bin/env bash
# Sets up building for Windows on Linux (./build.sh --windows): downloads xwin
# and, through it, Microsoft's C/C++ runtime and Windows SDK (headers and
# import libraries) into out/toolchain/xwin. Running it accepts Microsoft's
# license for those files (https://go.microsoft.com/fwlink/?LinkId=2086102);
# they can't be redistributed, so they are not in the repo.
#
#   tools/setup_windows_cross.sh               runtime + SDK 10.0.22621 (~600 MB)
#   tools/setup_windows_cross.sh --debug-libs  also the debug runtime (Debug builds)
#
# Also needed (Ubuntu/Debian packages): clang-20, lld-20, llvm-20 (llvm-rc,
# llvm-ar, llvm-mt), cmake, ninja-build, glslc.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="$REPO_ROOT/out/toolchain"
XWIN_VERSION=0.6.5
# 10.0.26100's package list has no x86_64 headers for xwin 0.6.5.
SDK_VERSION=10.0.22621
EXTRA=()
for arg in "$@"; do
  case "$arg" in
    --debug-libs) EXTRA+=(--include-debug-libs) ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "[!] Unknown argument: $arg" >&2; exit 1 ;;
  esac
done

mkdir -p "$DIR"
if [ ! -x "$DIR/xwin-bin" ]; then
  echo "[+] Downloading xwin $XWIN_VERSION"
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT
  name="xwin-$XWIN_VERSION-x86_64-unknown-linux-musl"
  curl -fsSL -o "$tmp/xwin.tar.gz" \
    "https://github.com/Jake-Shadle/xwin/releases/download/$XWIN_VERSION/$name.tar.gz"
  tar -xzf "$tmp/xwin.tar.gz" -C "$tmp"
  install -m 755 "$tmp/$name/xwin" "$DIR/xwin-bin"
fi
echo "[+] Downloading the Windows SDK $SDK_VERSION and the MSVC runtime (accepting Microsoft's license)"
"$DIR/xwin-bin" --accept-license --cache-dir "$DIR/xwin-cache" --sdk-version "$SDK_VERSION" \
  splat "${EXTRA[@]}" --output "$DIR/xwin"
rm -rf "$DIR/xwin-cache"  # the downloaded packages (~1 GB), unpacked already
# Windows finds headers and libraries case-insensitively: link the spellings
# the sources use (ObjBase.h for objbase.h).
python3 "$REPO_ROOT/tools/xwin_case_links.py" "$DIR/xwin" \
  "$REPO_ROOT/src" "$REPO_ROOT/tools" "$REPO_ROOT/thirdparty" "$REPO_ROOT/generated"
echo "[+] Ready: $DIR/xwin. Build with ./build.sh --windows"
