#!/usr/bin/env python3
"""r8f6v gen_adv.py: adversarial inputs for the f6 litlen sparse/dense dispatch.

The f6 commit 1 samples litlen symbols 0, 18, 36, ..., 270 (i*288/16) and
declines the sparse path when >= 6 of them are used; otherwise it lists all
288 symbols, and only then checks 2*used <= 288.  Data that uses many litlen
symbols but avoids the sampled byte values therefore pays for the full listing
and still takes master's dense path.

Files (2 MiB each, written to OUTDIR):
  avoid241  uniform random over the 241 byte values not in the sample
  avoid160  uniform random over 160 non-sampled values (dense: >144 used)
  s5mix     uniform over 150 non-sampled values + 5 sampled values
  rev140    140 non-sampled values, frequency decreasing with byte value
            (sparse path; the last counting-sort bucket is in reverse order,
            the worst case for the insertion sort at large blocks)
  rnd256    uniform random bytes (dense; the sample declines early)
"""
import os, random, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
N = 2 << 20
S = set(i * 288 // 16 for i in range(16))
non = [b for b in range(256) if b not in S]
assert len(non) == 241
r = random.Random(20261109)


def w(name, data):
    assert len(data) == N
    with open(os.path.join(out, name), 'wb') as f:
        f.write(bytes(data))


w('avoid241', [r.choice(non) for _ in range(N)])
a160 = non[:160]
w('avoid160', [r.choice(a160) for _ in range(N)])
samp5 = sorted(S - {270})[:5]
alpha = non[:150] + samp5
w('s5mix', [r.choice(alpha) for _ in range(N)])
v140 = non[:140]
# weights decreasing linearly from 2.2 to 1.0: in a 64 KiB block every value
# gets about 300-1000 occurrences (all >= 287, i.e. in the last bucket)
wts = [2.2 - 1.2 * i / 139 for i in range(140)]
w('rev140', r.choices(v140, weights=wts, k=N))
w('rnd256', [r.randrange(256) for _ in range(N)])
print('wrote', sorted(os.listdir(out)))
