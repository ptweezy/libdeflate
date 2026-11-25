#!/usr/bin/env python3
"""nooutline_ci padasm.py - test-only: move one function within a 128-byte line.

usage: padasm.py IN.s OUT.s FUNCTION K [COLD_SYMBOL...]

Copies clang's assembly IN.s to OUT.s.  Unless K is "none", it inserts
".p2align 7" and K bytes of nops right before FUNCTION's label, so that the
function starts K bytes past a 128-byte boundary, and ".p2align 7" right after
the function's .cfi_endproc, so that the code after it is placed the same for
every K.  The padding is never executed (it ends the previous function).

clang's assembly loses the Mach-O "cold function" flag (N_COLD_FUNC), which
the linker uses to move such functions to the end of __text; each
COLD_SYMBOL gets it back with ".desc SYMBOL, 0x400".
"""
import sys

src, dst, func, k = sys.argv[1:5]
cold = sys.argv[5:]
lines = open(src).read().split('\n')
label = func + ':'
starts = [n for n, l in enumerate(lines) if l.split(';')[0].strip() == label]
if len(starts) != 1:
    sys.exit('padasm: %d labels %r in %s' % (len(starts), label, src))
i = starts[0]
j = next((n for n in range(i, len(lines)) if lines[n].strip() == '.cfi_endproc'), None)
if j is None:
    sys.exit('padasm: no .cfi_endproc after %s' % label)
if k != 'none':
    k = int(k)
    if k % 4 or not 0 <= k < 128:
        sys.exit('padasm: K must be a multiple of 4 in [0, 128)')
    pad = ['\t.p2align\t7'] + ['\tnop'] * (k // 4)
    lines = lines[:i] + pad + lines[i:j + 1] + ['\t.p2align\t7'] + lines[j + 1:]
lines += ['\t.desc\t%s, 0x400' % s for s in cold]
open(dst, 'w').write('\n'.join(lines) + '\n')
