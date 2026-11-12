#!/usr/bin/env python3
"""r20probe.py BINDIR OUT.tsv TREES SEEDS FILES LEVEL ROUNDS REPS CORE
(test-only).  Runs `ldbench -A` for every (tree, seed, file), with and without
address-space randomization, in a new random order every round, each run as a
fresh copy of the binary in /dev/shm like lt.py.  Writes one line per run:
round, tree, seed, aslr, then ldbench's probe line."""
import os
import platform
import random
import shutil
import subprocess
import sys

bindir, out, trees, seeds, files, level, rounds, reps, core = sys.argv[1:10]
trees = trees.split(',')
if '-' in seeds:
    a, b = seeds.split('-')
    seeds = list(range(int(a), int(b) + 1))
else:
    seeds = [int(x) for x in seeds.split(',')]
files = files.split(',')
fresh = '/dev/shm/r20probe_%d' % os.getpid()
rng = random.Random(20)
noaslr = ['setarch', platform.machine(), '-R']
if subprocess.run(noaslr + ['true']).returncode:
    noaslr = None
n = 0
with open(out, 'w') as o:
    for rnd in range(int(rounds)):
        cells = [(t, s, f, r) for t in trees for s in seeds for f in files
                 for r in ((1, 0) if noaslr else (1,))]
        rng.shuffle(cells)
        for t, s, f, aslr in cells:
            b = os.path.join(bindir, t, 's%03d' % s, 'ldbench')
            shutil.copyfile(b, fresh)
            os.chmod(fresh, 0o755)
            cmd = ['taskset', '-c', core] + ([] if aslr else noaslr) + [
                fresh, '-A', str(rnd & 1), '-l', level, '-r', reps, f]
            p = subprocess.run(cmd, capture_output=True, text=True)
            os.unlink(fresh)
            if p.returncode or not p.stdout.startswith('probe '):
                sys.exit('%s: rc %d %s %s' % (' '.join(cmd), p.returncode,
                                              p.stdout[:200], p.stderr[:200]))
            o.write('%d\t%s\t%d\t%d\t%s' % (rnd, t, s, aslr, p.stdout))
            n += 1
        o.flush()
print('r20probe: %d runs, aslr-off %s' % (n, 'yes' if noaslr else 'unavailable'))
