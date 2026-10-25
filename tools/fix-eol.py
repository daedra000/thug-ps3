#!/usr/bin/env python3
"""Give changed files back the line endings they had at REV (most of kisak's sources are CRLF).

Usage: fix-eol.py REV [FILES...]   (default FILES: files changed since REV, plus unstaged changes)
"""
import subprocess, sys
rev = sys.argv[1]
files = sys.argv[2:] or subprocess.run(['git', 'diff', '--name-only', rev], capture_output=True, text=True).stdout.split()
for f in files:
    old = subprocess.run(['git', 'show', f'{rev}:{f}'], capture_output=True).stdout
    if not old:
        continue
    crlf = old.count(b'\r\n') > old.count(b'\n') // 2
    data = open(f, 'rb').read()
    lf = data.replace(b'\r\n', b'\n')
    new = lf.replace(b'\n', b'\r\n') if crlf else lf
    if new != data:
        open(f, 'wb').write(new)
        print('fixed', f, 'CRLF' if crlf else 'LF')
