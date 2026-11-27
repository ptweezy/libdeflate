#!/usr/bin/env python3
"""r5f3v gensyn.py OUTDIR -- extra inputs the track did not time, 2 MiB each:
abc (3 symbols uniform), skew2 (2 symbols, p = 0.95/0.05: long runs, tiny
literal alphabet, many length symbols), half (128 byte values uniform, no
matches), rnd256 (all byte values uniform), csv (lines of 6 random 7-digit
numbers), sparse (zeros with a random byte every ~40 bytes)."""
import os, random, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
N = 2 << 20
rng = random.Random(5031108)
files = {}
files['abc'] = bytes(rng.choice(b'abc') for _ in range(N))
files['skew2'] = bytes(rng.choices(b'01', weights=[95, 5], k=N))
files['half'] = bytes(rng.randrange(128) for _ in range(N))
files['rnd256'] = rng.randbytes(N)
b = bytearray()
while len(b) < N:
    b += (','.join(str(rng.randrange(10**6, 10**7)) for _ in range(6)) +
          '\n').encode()
files['csv'] = bytes(b[:N])
b = bytearray(N)
i = 0
while True:
    i += 1 + int(rng.expovariate(1 / 40))
    if i >= N:
        break
    b[i] = rng.randrange(1, 256)
files['sparse'] = bytes(b)
for k, v in files.items():
    open(os.path.join(out, k), 'wb').write(v)
    print(k, len(v), len(set(v)))
