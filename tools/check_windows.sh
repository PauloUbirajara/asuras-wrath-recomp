#!/usr/bin/env bash
# Compiles the game's, mantra's and mantra_shadergen's own sources against the
# Windows (MinGW) headers, syntax only, to catch Linux-only code without a
# Windows machine: POSIX calls, path.c_str() as const char* (wchar_t* there).
#
#   tools/check_windows.sh [build dir]   default out/build/linux-amd64-release
#
# Needs x86_64-w64-mingw32-g++ and a configured Linux build (for the include
# paths). The SDK itself and the XenosRecomp/DXC sources are not checked.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/out/build/linux-amd64-release}"
command -v x86_64-w64-mingw32-g++ >/dev/null || { echo "[!] x86_64-w64-mingw32-g++ not found" >&2; exit 1; }
cd "$BUILD_DIR"
ninja -t commands asura_wrath rexgpu-mantra mantra_shadergen | python3 -c '
import shlex, subprocess, sys
repo = sys.argv[1]
ours = (repo + "/src/", repo + "/tools/mantra_shadergen/main.cpp", repo + "/tools/mantra_shadergen/xxx_package.cpp")
seen, failed = set(), 0
for line in sys.stdin:
    if " -c " not in line:
        continue
    a = shlex.split(line)
    src = a[a.index("-c") + 1]
    if not src.startswith(ours) or src in seen or "platform_posix" in src:
        continue
    seen.add(src)
    flags, i = [], 1
    while i < len(a):
        x = a[i]
        if x == "-isystem":
            flags += [x, a[i + 1]]; i += 2; continue
        if x in ("-include", "-include-pch", "-Xclang", "-o", "-MF", "-MT", "-c"):
            i += 2; continue
        if x.startswith(("-I", "-D", "-std")) and "LINUX" not in x:
            flags.append(x)
        i += 1
    r = subprocess.run(["x86_64-w64-mingw32-g++", "-fsyntax-only", "-w", "-fpermissive", *flags, src],
                       capture_output=True, text=True)
    errors = [l for l in r.stderr.splitlines() if " error" in l]
    if errors:
        failed += 1
        print("== " + src[len(repo) + 1:])
        print("\n".join(errors[:6]))
print(f"[+] {len(seen)} files checked, {failed} with errors")
sys.exit(1 if failed else 0)
' "$REPO_ROOT"
