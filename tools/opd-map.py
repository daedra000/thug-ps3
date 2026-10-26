#!/usr/bin/env python3
"""Add code addresses to the symbol map, so crash addresses can be named.

usage: opd-map.py thug-ps3.elf thug-ps3.map    (rewrites the map, sorted)

On the PS3 (PPC64 ELFv1) a function's symbol is its descriptor in .opd, not its code: nm
lists descriptors only, while a crash's pc, lr and backtrace are code addresses. Each
descriptor's first 8 bytes are its code address; this adds a "code" line for each, so
lookup-addresses.py (or a search of the sorted map) finds the function a crash address
falls in. Based on reVC-PS3's ps3/tools/crashaddr.py.
"""
import re, struct, sys

elf, mapfile = sys.argv[1], sys.argv[2]
data = open(elf, 'rb').read()
shoff, = struct.unpack('>Q', data[0x28:0x30])
shentsize, shnum, shstrndx = struct.unpack('>HHH', data[0x3a:0x40])
secs = []
for i in range(shnum):
    h = data[shoff + i * shentsize: shoff + (i + 1) * shentsize]
    name, typ, flags, addr, off, size = struct.unpack('>IIQQQQ', h[:40])
    secs.append((name, addr, off, size))
strtab = secs[shstrndx]

def secname(n):
    s = data[strtab[2] + n:]
    return s[:s.index(b'\0')].decode()

opd = next((s for s in secs if secname(s[0]) == '.opd'), None)
lines = [l.rstrip('\n') for l in open(mapfile, errors='replace')]
out = list(lines)
if opd:
    seen = set()
    for line in lines:
        m = re.match(r'^([0-9a-fA-F]+)\s+(\S)\s+(.*)$', line)
        if not m:
            continue
        a = int(m.group(1), 16)
        if not (opd[1] <= a < opd[1] + opd[3]) or a in seen:
            continue
        seen.add(a)
        o = opd[2] + a - opd[1]
        code, = struct.unpack('>Q', data[o:o + 8])
        if code:
            out.append('%016x T %s (code)' % (code, m.group(3)))
out.sort()
open(mapfile, 'w').write('\n'.join(out) + '\n')
print('opd-map: %d descriptors given code addresses' % (len(out) - len(lines)))
