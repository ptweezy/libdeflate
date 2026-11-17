#!/usr/bin/env python3
"""mspadchk.py BINDIR NAME... (test-only)

For each NAME and each NAMEpP.exe that msbuildcm.bat linked with MSB_PADS,
checks in the .map files that every function of deflate_compress.obj is
exactly P bytes above its address in NAME.exe, i.e. that the padding moved
libdeflate's code without changing it.  Exits 1 otherwise.
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
    for path in sorted(glob.glob(os.path.join(bindir, name + 'p*.map'))):
        p = int(re.search(r'p(\d+)\.map$', path).group(1))
        d = funcs(path)
        moved = {f: d.get(f, -1) - a for f, a in base.items()}
        wrong = {f: v for f, v in moved.items() if v != p}
        print('%s: %d functions, moved by %s%s' % (
            os.path.basename(path), len(base), sorted(set(moved.values())),
            '' if not wrong else '  WRONG: ' + ', '.join(sorted(wrong)[:5])))
        bad += bool(wrong)
sys.exit(1 if bad else 0)
