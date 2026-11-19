#!/usr/bin/env python3
"""Windows headers and libraries are found case-insensitively; Linux file
systems aren't. For every #include name in the sources (and every library
name in the CMake files) that matches a file of the xwin SDK only when case is
ignored ("ObjBase.h" for objbase.h, "Shlwapi.lib"), adds a symlink with the
spelling used. Run by tools/setup_windows_cross.sh; safe to rerun.

    tools/xwin_case_links.py <xwin dir> <source dir>...
"""
import os
import re
import sys

INCLUDE = re.compile(rb'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
PRAGMA_LIB = re.compile(rb'#\s*pragma\s+comment\s*\(\s*lib\s*,\s*"([^"]+)"', re.I)
CMAKE_WORD = re.compile(rb'[A-Za-z0-9_.+-]+')
SOURCE_EXT = {'.h', '.hh', '.hpp', '.hxx', '.inl', '.inc', '.c', '.cc', '.cpp', '.cxx', '.rc'}


def scan(roots):
    includes, libs = set(), set()
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in ('.git', 'out', 'build')]
            for name in filenames:
                path = os.path.join(dirpath, name)
                ext = os.path.splitext(name)[1].lower()
                try:
                    if ext in SOURCE_EXT:
                        data = open(path, 'rb').read()
                        includes.update(m.decode('latin-1') for m in INCLUDE.findall(data))
                        libs.update(m.decode('latin-1') for m in PRAGMA_LIB.findall(data))
                    elif name == 'CMakeLists.txt' or ext == '.cmake':
                        data = open(path, 'rb').read()
                        libs.update(w.decode('latin-1') for w in CMAKE_WORD.findall(data))
                except OSError:
                    pass
    return includes, libs


def index(dirs):
    """lower-case relative path -> real relative path, per directory."""
    out = []
    for d in dirs:
        table = {}
        for dirpath, _, filenames in os.walk(d):
            for name in filenames:
                rel = os.path.relpath(os.path.join(dirpath, name), d)
                table.setdefault(rel.lower(), rel)
        out.append((d, table))
    return out


def link(base, wanted, real):
    """Symlink base/wanted -> base/real, creating case-variant folders too."""
    target = os.path.join(base, wanted)
    if os.path.lexists(target):
        return False
    parent = os.path.dirname(wanted)
    real_parent = os.path.dirname(real)
    if parent and not os.path.isdir(os.path.join(base, parent)):
        os.symlink(os.path.relpath(os.path.join(base, real_parent), os.path.join(base, os.path.dirname(parent)) or base),
                   os.path.join(base, parent))
        return True
    os.symlink(os.path.relpath(os.path.join(base, real), os.path.dirname(target)), target)
    return True


def main():
    xwin, roots = sys.argv[1], sys.argv[2:]
    includes, words = scan(roots)
    inc_dirs = [os.path.join(xwin, p) for p in
                ('crt/include', 'sdk/include/ucrt', 'sdk/include/um', 'sdk/include/shared',
                 'sdk/include/winrt')]
    lib_dirs = [os.path.join(xwin, p) for p in
                ('crt/lib/x86_64', 'sdk/lib/um/x86_64', 'sdk/lib/ucrt/x86_64')]
    made = 0
    for base, table in index([d for d in inc_dirs if os.path.isdir(d)]):
        for name in includes:
            name = name.replace('\\', '/')
            real = table.get(name.lower())
            if real and real != name and link(base, name, real):
                made += 1
    libs = {w if w.lower().endswith('.lib') else w + '.lib' for w in words}
    for base, table in index([d for d in lib_dirs if os.path.isdir(d)]):
        for name in libs:
            real = table.get(name.lower())
            if real and real != name and '/' not in name and link(base, name, real):
                made += 1
    print(f'[+] {made} case-variant links in {xwin}')


if __name__ == '__main__':
    main()
