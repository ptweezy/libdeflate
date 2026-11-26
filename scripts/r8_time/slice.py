#!/usr/bin/env python3
"""r8_time slice.py: the deterministic 2 MiB slice of a corpus file.

usage: slice.py IN OUT

OUT = four 512 KiB pieces of IN, starting at offsets (i * size) // 4 for
i = 0..3, concatenated.  This reproduces the 13 local benchmark slices
(~/corpus/slices-2MiB: 12 Silesia files + enwik8) bit for bit; inputs.sh
checks the result against their SHA-256 (slices.sha256).  A file shorter
than 2 MiB is copied whole.
"""
import sys

PIECE = 512 << 10
src, dst = sys.argv[1], sys.argv[2]
with open(src, 'rb') as f:
    data = f.read()
n = len(data)
if n <= 4 * PIECE:
    out = data
else:
    out = b''.join(data[(i * n) // 4:(i * n) // 4 + PIECE] for i in range(4))
with open(dst, 'wb') as f:
    f.write(out)
