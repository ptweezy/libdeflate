#!/usr/bin/env python3
"""mslayout.py BINDIR EXPECT  (test-only)

EXPECT: comma-separated NAME:D1[:D2].  Reads BINDIR/NAME.map, prints the
addresses of a few functions of deflate_compress.obj, and fails unless
deflate_find_min_cost_path_simd of each NAME is D1 bytes above that of the
first NAME, deflate_flush_block is D2 bytes above (D2 defaults to D1), and
deflate_compress_near_optimal is at the same address.  A non-numeric D1 means
print only.
"""
import os, re, sys

SYMS = ['deflate_compress_near_optimal', 'deflate_find_min_cost_path',
        'deflate_find_min_cost_path_simd', 'deflate_flush_block',
        'deflate_optimize_and_flush_block']
bindir = sys.argv[1]
expect = []
for x in sys.argv[2].split(','):
    p = x.split(':')
    d1 = int(p[1]) if p[1].isdigit() else None
    expect.append((p[0], d1, int(p[2]) if len(p) > 2 else d1))


def load(name):
    d = {}
    for line in open(os.path.join(bindir, name + '.map'), errors='replace'):
        m = re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s', line)
        if m:
            d[m.group(1)] = int(m.group(2), 16)
    return d


maps = {e[0]: load(e[0]) for e in expect}
first = maps[expect[0][0]]
bad = 0
for n, d1, d2 in expect:
    m = maps[n]
    print(n, ' '.join('%s=%s' % (s.replace('deflate_', ''),
                                 '%x' % m[s] if s in m else '-') for s in SYMS))
    if d1 is None:
        continue
    for k, want in (('deflate_find_min_cost_path_simd', d1),
                    ('deflate_flush_block', d2),
                    ('deflate_compress_near_optimal', 0)):
        if m[k] - first[k] != want:
            print('  UNEXPECTED: %s moved %d, wanted %d' % (k, m[k] - first[k], want))
            bad += 1
sys.exit(1 if bad else 0)
