#!/usr/bin/env python3
"""r8f6v gen_thr.py: inputs around the f6 sparse/dense dispatch threshold.

Each file is a sequence of 4 KiB segments.  Every segment draws its bytes
from an alphabet of k byte values, of which h are litlen symbols sampled by
list_used_litlen_symbols() (0, 18, ..., 252), and copies earlier data with
probability p per position (lengths 3..258, distances 1..segment start), so
that length and offset symbols are used too.  k sweeps 100..190 (the litlen
code then uses about k+1 plus the length symbols, i.e. both sides of 144),
h sweeps 0..9 (both sides of the 6-of-16 early exit), p in {0, .002, .02,
.1, .3}.  Skewed frequencies (half the segments) make the last counting-sort
bucket large at 64 KiB blocks.
"""
import os, random, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
S = [i * 288 // 16 for i in range(16) if i * 288 // 16 < 256]
non = [b for b in range(256) if b not in S]
r = random.Random(424242)


def segment(n, k, h, p, skew):
    h = min(h, len(S), k)
    alpha = r.sample(S, h) + r.sample(non, k - h)
    if skew:
        wts = [1.0 / (1 + i) ** 0.7 for i in range(k)]
    else:
        wts = None
    buf = bytearray()
    while len(buf) < n:
        if buf and p and r.random() < p:
            ln = min(r.randint(3, 40) if r.random() < .9 else r.randint(3, 258), n - len(buf))
            d = r.randint(1, min(len(buf), 32768))
            for _ in range(ln):
                buf.append(buf[-d])
        else:
            buf.append(r.choices(alpha, weights=wts)[0] if wts else r.choice(alpha))
    return bytes(buf[:n])


for name, seglen, ks in [('thr4k', 4096, range(100, 191, 3)),
                         ('thr1k', 1024, range(120, 171, 2)),
                         ('thr64k', 65536, range(120, 171, 10))]:
    data = bytearray()
    for k in ks:
        for h in range(0, 10, 1 if seglen <= 4096 else 3):
            for p in (0, .002, .02, .1, .3):
                if seglen == 65536 and p not in (0, .02):
                    continue
                data += segment(seglen, k, h, p, skew=r.random() < .5)
    with open(os.path.join(out, name), 'wb') as f:
        f.write(data)
    print(name, len(data))
