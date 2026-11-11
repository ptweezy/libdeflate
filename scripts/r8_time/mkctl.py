#!/usr/bin/env python3
"""r8_time mkctl.py: control trees derived from the base tree (test-only;
the CI port of handoff/tools/layout-harness/mkctl.py, x86 and aarch64).

  posK  : + a dependent multiply chain of in_nbytes/K iterations at the end
          of libdeflate_deflate_compress(): a real, known cost outside the
          parsers, so their code and placement are untouched.  One iteration
          is 3 cycles on x86 (imul) and 2-3 on Arm cores (mul); `ldbench -K`
          times the same loop, so the cost is predicted from a measurement
          on the machine itself instead of from an assumed latency.
  plc1  : + a jump over 200 unused bytes at the top of
          deflate_near_optimal_init_stats() (never executed at L1-L9; in a
          plain build it moves every function after it by ~208 B)
  plc2  : the same at the top of deflate_compress_greedy()'s body (never
          executed at L1 or L5-L12)
  plc3  : the same inside deflate_compress_fastest() (one jump per call;
          moves the L1 parser's loops inside the function, so it is NOT a
          placement-only control in anchor mode: it shows what a change
          inside a hot function costs in variance)

usage: mkctl.py SRCROOT BASE NAME...   (e.g. mkctl.py $R8LAY_SRC m0 pos64 plc1)
Creates SRCROOT/NAME-src from SRCROOT/BASE-src.
"""
import os, shutil, sys

JMP = ('#if defined(__aarch64__)\n'
       '\t__asm__ volatile("b 1f\\n\\t.skip 200, 0\\n1:");\n'
       '#else\n'
       '\t__asm__ volatile("jmp 1f\\n\\t.skip 200, 0xcc\\n1:");\n'
       '#endif\n')


def edit(path, old, new):
    s = open(path).read()
    if s.count(old) != 1:
        sys.exit('pattern not unique in %s: %r' % (path, old))
    open(path, 'w').write(s.replace(old, new))


if len(sys.argv) < 4:
    sys.exit(__doc__)
root, base = sys.argv[1], sys.argv[2]
SRC = os.path.join(root, base + '-src')
for name in sys.argv[3:]:
    dst = os.path.join(root, '%s-src' % name)
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(SRC, dst)
    f = os.path.join(dst, 'lib', 'deflate_compress.c')
    if name.startswith('pos'):
        k = int(name[3:])
        edit(f, '\t(*c->impl)(c, in, in_nbytes, &os);\n',
             '\t(*c->impl)(c, in, in_nbytes, &os);\n'
             '\t{\n'
             '\t\t/* r8lay positive control: one multiply per %d bytes */\n'
             '\t\tsize_t r8n = in_nbytes / %d;\n'
             '\t\tunsigned long r8x = 1;\n'
             '\n'
             '\t\twhile (r8n--)\n'
             '#if defined(__aarch64__)\n'
             '\t\t\t__asm__ volatile("mul %%0, %%0, %%0" : "+r"(r8x));\n'
             '#else\n'
             '\t\t\t__asm__ volatile("imul %%0, %%0" : "+r"(r8x));\n'
             '#endif\n'
             '\t\t__asm__ volatile("" : : "r"(r8x));\n'
             '\t}\n' % (k, k))
    elif name == 'plc1':
        edit(f, 'deflate_near_optimal_init_stats(struct libdeflate_compressor *c)\n{\n',
             'deflate_near_optimal_init_stats(struct libdeflate_compressor *c)\n{\n' + JMP)
    elif name == 'plc2':
        edit(f, '\tu32 next_hashes[2] = {0, 0};\n\n\thc_matchfinder_init(&c->p.g.hc_mf, GREEDY_MULTI_HOP);\n',
             '\tu32 next_hashes[2] = {0, 0};\n\n' + JMP + '\thc_matchfinder_init(&c->p.g.hc_mf, GREEDY_MULTI_HOP);\n')
    elif name == 'plc3':
        edit(f, '\tu32 next_hash = 0;\n\n\tht_matchfinder_init(&c->p.f.ht_mf);\n',
             '\tu32 next_hash = 0;\n\n' + JMP + '\tht_matchfinder_init(&c->p.f.ht_mf);\n')
    else:
        sys.exit('unknown control ' + name)
    print('made', dst)
