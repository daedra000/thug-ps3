#!/usr/bin/env python3
"""Make the Windows-cased #includes in Code/ resolve on case-sensitive file systems.

The original code was written for Windows, where `#include <core/defines.h>` finds
`Code/Core/Defines.h`. This scans every source and header for #include lines, resolves
each spelling case-insensitively, and creates a symlink tree under OUT_DIR so the exact
spelling exists. Add OUT_DIR to the include path (after Code/).

Quoted includes are resolved against the including file's directory first, as the
compiler does; since quoted includes fall back to the -I paths, the link for a
mis-cased one is created under OUT_DIR/<spelling> too.

Usage: gen-case-includes.py CODE_DIR OUT_DIR
"""
import os
import re
import sys

INC_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
SKIP_DIRS = {'gamespy', 'ngps', 'ngc'}
SRC_EXT = ('.c', '.cpp', '.h', '.hpp', '.inl', '.inc')


def build_index(root):
    """Map lowercased relative path -> real relative path for every file under root."""
    index = {}
    for dirpath, dirnames, filenames in os.walk(root):
        for name in filenames:
            real = os.path.relpath(os.path.join(dirpath, name), root)
            index.setdefault(real.lower(), real)
    return index


def norm(path):
    parts = []
    for p in path.replace('\\', '/').split('/'):
        if p in ('', '.'):
            continue
        if p == '..':
            if parts:
                parts.pop()
            continue
        parts.append(p)
    return '/'.join(parts)


def main():
    code, out = sys.argv[1], sys.argv[2]
    code = os.path.abspath(code)
    out = os.path.abspath(out)
    index = build_index(code)
    search_roots = ['', 'Sk', 'Ps3/include']  # mirrors the -I order in Makefile.ps3

    links = {}       # spelling (relative to out) -> real relative path
    backslashed = set()
    unresolved = set()

    for dirpath, dirnames, filenames in os.walk(code):
        # Skip code the PS3 build never compiles (other consoles, GameSpy samples).
        dirnames[:] = [d for d in dirnames if d.lower() not in SKIP_DIRS]
        for name in filenames:
            if not name.lower().endswith(SRC_EXT):
                continue
            src = os.path.join(dirpath, name)
            try:
                text = open(src, encoding='latin-1').read()
            except OSError:
                continue
            src_dir = os.path.relpath(dirpath, code)
            for kind, inc in INC_RE.findall(text):
                if '\\' in inc:
                    backslashed.add(inc)
                spelling = inc.replace('\\', '/')
                candidates = []
                if kind == '"':
                    candidates.append(norm(os.path.join(src_dir, spelling)))
                candidates += [norm(os.path.join(r, spelling)) for r in search_roots]
                hit = None
                for c in candidates:
                    if os.path.exists(os.path.join(code, c)):
                        hit = 'exact'
                        break
                    real = index.get(c.lower())
                    if real:
                        hit = real
                        # Link at the spelling relative to the matching search root, or,
                        # for a quoted relative include, at a path the -I tree can serve.
                        if c.startswith(src_dir + '/') and kind == '"':
                            key = spelling if '..' not in spelling else None
                        else:
                            key = spelling
                        if key:
                            prev = links.get(key)
                            if prev and prev != real:
                                print(f'warning: {key} is ambiguous ({prev} vs {real})', file=sys.stderr)
                            else:
                                links[key] = real
                        break
                if hit is None and not spelling.startswith(('sys/', 'rsx/', 'io/', 'audio/')):
                    unresolved.add(spelling)

    # GCC takes '\\' in an #include literally (clang and MSVC treat it as a separator),
    # so a backslashed spelling gets a link with that exact (flat) name as well.
    for raw in list(backslashed):
        target = links.get(raw.replace('\\', '/')) or index.get(norm(raw).lower())
        if target:
            links[raw] = target

    # A header reached through a link sees its quoted includes relative to the link's
    # directory, so give each linked header's quoted includes links there as well.
    pending = list(links.items())
    while pending:
        key, real = pending.pop()
        try:
            text = open(os.path.join(code, real), encoding='latin-1').read()
        except OSError:
            continue
        for kind, inc in INC_RE.findall(text):
            if kind != '"':
                continue
            spelling = inc.replace('\\', '/')
            target = index.get(norm(os.path.join(os.path.dirname(real), spelling)).lower())
            if not target:
                continue
            new_key = norm(os.path.join(os.path.dirname(key), spelling))
            if new_key and new_key not in links:
                links[new_key] = target
                pending.append((new_key, target))

    count = 0
    for key, real in links.items():
        dst = os.path.join(out, key)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        target = os.path.join(code, real)
        if os.path.islink(dst) or os.path.exists(dst):
            if os.path.islink(dst) and os.readlink(dst) == target:
                continue
            os.remove(dst)
        os.symlink(target, dst)
        count += 1
    print(f'{len(links)} case-fixed include links ({count} new) in {out}')


if __name__ == '__main__':
    main()
