#!/usr/bin/env python3
"""Turn GCC/ld errors in a build log into GitHub Actions annotations.

Prints a summary, then up to MAX `::error` lines (GitHub shows 10 per step in the
UI, but all of them are available through the check-runs API). Exits 0; the build
step's own status says whether it failed.

Usage: ci-annotate.py build.log [repo_root]
"""
import os
import re
import sys

MAX = 50

log_path = sys.argv[1]
root = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.getcwd()

compile_re = re.compile(r'^(?P<file>[^:\s][^:]*):(?P<line>\d+):(?:(?P<col>\d+):)?\s*(?:fatal )?error:\s*(?P<msg>.*)$')
link_re = re.compile(r'(undefined reference to `(?P<sym>[^\']+)\'|multiple definition of `(?P<dup>[^\']+)\')')

errors = []
undefined = {}
seen = set()
with open(log_path, errors='replace') as f:
    for line in f:
        line = line.rstrip('\n')
        m = compile_re.match(line)
        if m:
            path = m.group('file')
            if path.startswith(root + '/'):
                path = path[len(root) + 1:]
            key = (path, m.group('line'), m.group('msg'))
            if key in seen:
                continue
            seen.add(key)
            errors.append(key)
            continue
        m = link_re.search(line)
        if m:
            sym = m.group('sym') or m.group('dup')
            kind = 'undefined' if m.group('sym') else 'multiple definition'
            undefined.setdefault((kind, sym), 0)
            undefined[(kind, sym)] += 1

files = sorted({e[0] for e in errors})
print(f'{len(errors)} compile errors in {len(files)} files, {len(undefined)} link problems')

budget = MAX
for path, line, msg in errors:
    if budget == 0:
        break
    msg = msg.replace('%', '%25').replace('\r', '').replace('\n', ' ')
    print(f'::error file={path},line={line}::{msg}')
    budget -= 1
for (kind, sym), count in sorted(undefined.items(), key=lambda kv: -kv[1]):
    if budget == 0:
        break
    print(f'::error title=link::{kind}: {sym} ({count}x)')
    budget -= 1

if errors or undefined:
    with open(os.environ.get('GITHUB_STEP_SUMMARY', os.devnull), 'a') as s:
        s.write(f'### PS3 build: {len(errors)} compile errors, {len(undefined)} link problems\n\n')
        for path in files[:200]:
            s.write(f'- {path}\n')
