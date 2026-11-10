#!/usr/bin/env python3
"""r8_time gen_z3.py: inputs for locating the rev140 loss (2 MiB each).
  preK    like gen_adv.py's rev140, with the first K non-sampled byte values
          (K = 40, 70, 100, 120): the used litlen symbols are one long run,
          so master's "is this symbol used" branch predicts well
  alt128  uniform over the even byte values: used and unused alternate
"""
import os, random, sys
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
N = 2 << 20
S = set(i * 288 // 16 for i in range(16))
non = [b for b in range(256) if b not in S]
for k in (40, 70, 100, 120):
    r = random.Random(20261110 + k)
    wts = [2.2 - 1.2 * i / (k - 1) for i in range(k)]
    with open(os.path.join(out, 'pre%d' % k), 'wb') as f:
        f.write(bytes(r.choices(non[:k], weights=wts, k=N)))
r = random.Random(20261110)
with open(os.path.join(out, 'alt128'), 'wb') as f:
    f.write(bytes(2 * r.randrange(128) for _ in range(N)))
print('wrote', sorted(os.listdir(out)))
