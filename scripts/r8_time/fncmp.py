#!/usr/bin/env python3
# usage: fncmp.py CC TREE_A TREE_B [extra cflags...]
# Compile lib/deflate_compress.c of both trees, compare each function's code.
# Report: same, same-but-offsets (only struct displacements differ), or differs.
import os, re, subprocess, sys, tempfile
cc, A, B = sys.argv[1:4]
extra = sys.argv[4:]
tmp = tempfile.mkdtemp(dir=os.environ.get("TMPDIR", "/tmp"))
def build(tree, tag):
    o = os.path.join(tmp, tag + '.o')
    subprocess.run([cc, '-O2', '-DNDEBUG', '-std=gnu11', '-ffunction-sections', '-c',
                    '-I', tree, os.path.join(tree, 'lib/deflate_compress.c'), '-o', o] + extra, check=True)
    out = subprocess.run(['objdump', '-dr', '--no-show-raw-insn', '--no-addresses', o],
                         capture_output=True, text=True, check=True).stdout
    funcs, cur = {}, None
    for line in out.splitlines():
        m = re.match(r'^<(.+)>:$', line.strip())
        if m:
            cur = m.group(1); funcs[cur] = []; continue
        if cur is None or not line.strip() or line.startswith('Disassembly'):
            continue
        t = line.strip()
        t = re.sub(r'\s+', ' ', t)
        t = re.sub(r'\.LCPI\d+_\d+', '.LCPI', t)
        t = re.sub(r'\.LC\d+\b', '.LC', t)
        t = re.sub(r'\b[0-9a-f]+ <', '<', t)          # branch targets: keep symbolic part
        funcs[cur].append(t)
    return funcs
fa, fb = build(A, 'a'), build(B, 'b')
def nooff(lines):
    return [re.sub(r'-?0x[0-9a-f]+\(', 'D(', re.sub(r'\$0x[0-9a-f]+', '$I', l)) for l in lines]
interesting = [f for f in sorted(set(fa) | set(fb))]
for f in interesting:
    a, b = fa.get(f), fb.get(f)
    if a is None or b is None:
        print('%-50s only in %s (%d insns)' % (f, 'A' if b is None else 'B', len(a or b)))
        continue
    if a == b:
        st = 'same'
    elif nooff(a) == nooff(b):
        st = 'same except displacements/immediates'
    else:
        st = 'DIFFERS'
    if st != 'same' or os.environ.get('ALL'):
        print('%-50s %-38s %d -> %d insns' % (f, st, len(a), len(b)))
