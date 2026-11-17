#!/usr/bin/env python3
"""mspadchk.py BINDIR NAME... (test-only)

For each NAME and each NAMEpP.exe that msbuildcm.bat linked with MSB_PADS,
checks in the .map files that every function of deflate_compress.obj moved by
the same number of bytes from its address in NAME.exe, i.e. that the padding
moved libdeflate's code without changing it, and that the padded builds of
each NAME put the code at distinct offsets modulo 64.  (The move is P plus
whatever alignment the linker adds, which differs between targets.)  Exits 1
otherwise.
"""
import glob, os, re, sys

def funcs(path):
    d = {}
    for line in open(path, errors='replace'):
        m = re.match(r'\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{8,16})\s+f\s+(?:i\s+)?(\S+)', line)
        if m and 'deflate_compress.obj' in m.group(3):
            d[m.group(1)] = int(m.group(2), 16)
    return d

bindir, names = sys.argv[1], sys.argv[2:]
bad = 0
for name in names:
    base = funcs(os.path.join(bindir, name + '.map'))
    assert len(base) > 20, (name, len(base))
    mods = []
    for path in sorted(glob.glob(os.path.join(bindir, name + 'p*.map'))):
        d = funcs(path)
        moved = sorted({d.get(f, -1) - a for f, a in base.items()})
        ok = len(moved) == 1 and moved[0] > 0
        print('%s: %d functions, moved by %s%s' % (
            os.path.basename(path), len(base), moved, '' if ok else '  NOT UNIFORM'))
        bad += not ok
        mods.append(moved[0] % 64)
    if len(set(mods)) != len(mods):
        print('%s: offsets modulo 64 repeat: %s' % (name, mods))
        bad += 1
sys.exit(1 if bad else 0)
