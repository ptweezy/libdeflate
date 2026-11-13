#!/usr/bin/env python3
"""msab.py BINDIR ROUNDS REPS OUT.tsv NAMES CFGS -- FILES  (test-only)

NAMES: comma-separated exe names in BINDIR; CFGS: comma-separated LEVEL:CHUNK.
Runs the exes interleaved, with the order rotated every round, keeps each
exe's minimum ns/byte per (cfg, file), checks that every exe produced the
same output hash, and writes one TSV row per (cfg, file, exe).
"""
import glob, os, shutil, subprocess, sys, time

bindir, rounds, reps, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
names = sys.argv[5].split(',')
cfgs = sys.argv[6].split(',')
files = []
for pat in sys.argv[sys.argv.index('--') + 1:]:
    files += sorted(glob.glob(pat))
assert files, 'no input files'

FRESH = os.environ.get('MSAB_FRESH') == '1'
nrun = 0

def run(name, lvl, chunk):
    # MSAB_FRESH=1: run a new copy of the exe every time, so that each run
    # gets new physical pages for the image (one file keeps its pages, and
    # with them a fast or slow placement, for the whole job).
    global nrun
    exe = os.path.join(bindir, name + '.exe')
    tmp = None
    if FRESH:
        nrun += 1
        tmp = os.path.join(bindir, 'fresh-%d-%s.exe' % (nrun, name))
        shutil.copyfile(exe, tmp)
        exe = tmp
    try:
        res = subprocess.run([exe, lvl, chunk, str(reps)] + files,
                             capture_output=True, text=True, check=True).stdout
    finally:
        if tmp:
            try:
                os.remove(tmp)
            except OSError:
                pass
    d = {}
    for line in res.splitlines():
        f, t, sz, h = line.rsplit(None, 3)
        d[f] = (float(t), sz + ':' + h)
    return d

bad = 0
t_start = time.time()
with open(out, 'w') as tsv:
    for cfg in cfgs:
        lvl, _, chunk = cfg.partition(':')
        chunk = chunk or '0'
        best, allt, sig = {}, {}, {}
        for r in range(rounds):
            k = r % len(names)
            order = names[k:] + names[:k]
            if (r // len(names)) % 2:
                order.reverse()
            for name in order:
                for f, (t, s) in run(name, lvl, chunk).items():
                    allt.setdefault((name, f), []).append(t)
                    if (name, f) not in best or t < best[(name, f)]:
                        best[(name, f)] = t
                    sig.setdefault(f, set()).add(s)
        for f in files:
            same = len(sig[f]) == 1
            bad += not same
            for name in names:
                tsv.write('%s\t%s\t%s\t%s\t%.3f\t%s\t%s\n' % (
                    lvl, chunk, os.path.basename(f), name, best[(name, f)],
                    ','.join('%.3f' % t for t in allt[(name, f)]),
                    'same' if same else 'DIFF'))
            print('L%s:%s %-10s %s  %s' % (
                lvl, chunk, os.path.basename(f),
                ' '.join('%s=%.2f' % (n, best[(n, f)]) for n in names),
                'same' if same else 'OUTPUT DIFF'), flush=True)
        tsv.flush()
        print('# cfg %s done at %.0f s' % (cfg, time.time() - t_start), flush=True)
print('output mismatches: %d' % bad)
sys.exit(1 if bad else 0)
