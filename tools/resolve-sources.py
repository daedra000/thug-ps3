#!/usr/bin/env python3
"""Print kisak-thug's source list (cmake_files.cmake) with each path's real letter case.

The list was written on Windows, so some entries don't match the file names
(e.g. Core/Math/Geometry.cpp is really Core/Math/geometry.cpp). Paths are printed
relative to Code/; entries with no matching file are skipped.

Usage: resolve-sources.py ROOT
"""
import os
import re
import sys

root = sys.argv[1]
code = os.path.join(root, 'Code')

index = {}
for dirpath, _, files in os.walk(code):
    for f in files:
        rel = os.path.relpath(os.path.join(dirpath, f), code)
        index.setdefault(rel.lower(), rel)

pattern = re.compile(r'"\$\{SRC_DIR\}/([^"]+\.(?:cpp|c))"')
seen = set()
with open(os.path.join(root, 'cmake_files.cmake'), encoding='latin-1') as f:
    for line in f:
        for m in pattern.finditer(line):
            real = index.get(m.group(1).replace('\\', '/').lower())
            if real and real not in seen:
                seen.add(real)
                print(real)
