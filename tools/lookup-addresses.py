#!/usr/bin/env python3
"""Name addresses from a console log using the build's symbol map (nm output).

usage: lookup-addresses.py thug-ps3.map addresses.txt
addresses.txt: one hex address per line (anything after it is a note). Function pointers on
the PS3 point at function descriptors (.opd, nm type D/d), which nm names like the function.
Prints a GitHub Actions notice per address.
"""
import bisect, re, sys

syms = []
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'^([0-9a-fA-F]+)\s+(\S)\s+(.*)$', line.rstrip('\n'))
    if m:
        syms.append((int(m.group(1), 16), m.group(2), m.group(3)))
syms.sort()
addrs = [s[0] for s in syms]

for line in open(sys.argv[2]):
    m = re.match(r'^\s*(0x)?([0-9a-fA-F]+)\b(.*)$', line)
    if not m:
        continue
    a = int(m.group(2), 16)
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0:
        print(f'::notice title=address 0x{a:x}::no symbol at or below it')
        continue
    # Every symbol at that exact address (a descriptor and its function can share one).
    base = syms[i][0]
    names = [f'{t} {n}' for (ad, t, n) in syms if ad == base][:4]
    print(f'::notice title=address 0x{a:x}{m.group(3).rstrip()}::+0x{a - base:x} from ' + ' | '.join(names))
