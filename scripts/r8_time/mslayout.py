#!/usr/bin/env python3
"""mslayout.py BINDIR EXPECT  (test-only)

EXPECT: comma-separated NAME:DELTA.  Reads BINDIR/NAME.map, prints the
addresses of a few functions of deflate_compress.obj, and fails unless
deflate_find_min_cost_path_simd of each NAME is DELTA bytes above that of the
first NAME while deflate_compress_near_optimal is at the same address in all.
"""
import os, re, sys

SYMS = ['deflate_compress_near_optimal', 'deflate_find_min_cost_path',
        'deflate_find_min_cost_path_pad', 'deflate_find_min_cost_path_simd',
        'deflate_flush_block', 'deflate_optimize_and_flush_block']
bindir = sys.argv[1]
expect = [(n, int(d)) for n, d in (x.split(':') for x in sys.argv[2].split(','))]


def load(name):
    d = {}
    for line in open(os.path.join(bindir, name + '.map'), errors='replace'):
        m = re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s', line)
        if m:
            d[m.group(1)] = int(m.group(2), 16)
    return d


maps = {n: load(n) for n, _ in expect}
first = maps[expect[0][0]]
bad = 0
for n, delta in expect:
    m = maps[n]
    print(n, ' '.join('%s=%s' % (s.replace('deflate_', ''),
                                 '%x' % m[s] if s in m else '-') for s in SYMS))
    k = 'deflate_find_min_cost_path_simd'
    if m[k] - first[k] != delta:
        print('  UNEXPECTED shift %d, wanted %d' % (m[k] - first[k], delta))
        bad += 1
    k = 'deflate_compress_near_optimal'
    if m[k] != first[k]:
        print('  UNEXPECTED: %s moved' % k)
        bad += 1
sys.exit(1 if bad else 0)
