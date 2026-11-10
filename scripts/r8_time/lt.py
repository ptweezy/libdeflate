#!/usr/bin/env python3
"""r8_time lt.py - interleaved timing of randomized-layout builds (lb.py);
test-only, the CI port of handoff/tools/layout-harness/lt.py.

usage: lt.py -o OUT.json -b BINDIR -t TREE0,TREE1,... --seeds 1-32
             --files dir:DIR|PATH,PATH,... [-l LEVEL] [-s CHUNK] [-n ROUNDS]
             [-r REPS] [--core C] [--driver-cpus LIST] [--aa] [--budget SECS]
             [--fresh-dir DIR] [--no-fresh] [--calib-every N] [--order perm|rot]

BINDIR is $R8LAY_ROOT-b/CFG.MODE (or an absolute path).  A variant is
(tree, seed): BINDIR/TREE/sNNN/ldbench.  --aa adds tree "AA": a
byte-identical copy of TREE0's binary for every seed (a separate file), for
the copy A/A.

Each round visits every (seed, file) pair in a new random order and runs
every tree once for that pair, back to back.  Each run is one process
pinned to --core: `ldbench -x`, which reports the best of REPS for three
clocks (wall nanoseconds, thread CPU nanoseconds, user-mode cycles if the
machine has a PMU), per input byte.  "data" holds the wall-clock values,
"data_cpu" and "data_cyc" the others; one value per round, in round order.
Every run's compressed-output hash is checked against the first one for
that file (all trees, all seeds).

Order of the trees within a visit:
  perm (default)  a new random permutation for every visit, so that a
                  tree's position and its predecessor are random.
  rot             the local lt.py's rule: rotated by round and pair, and
                  reversed on odd rounds.  Positions are balanced exactly,
                  but the neighbours are fixed: the base tree always runs
                  right after its copy "AA" in even rounds and right before
                  it in odd ones.  The first runs on Arm runners (cycle
                  counters, per-seed sd of 0.01-0.05%) showed copy A/As of
                  -0.02..-0.07% in one-round jobs, i.e. a process appears
                  to run slightly faster right after an identical binary;
                  with random orders such an effect is noise, not bias.
"visits" records [round, seed, file index, order] for every visit (order =
the trees' indexes in "trees"); ordchk.py measures position and
predecessor effects from it.

Fresh copies (default): every run executes a new copy of the binary in
--fresh-dir (/dev/shm), deleted afterwards, so that each process gets new
page-cache pages for its code (a binary file can keep a slow physical
placement for its whole life).

Calibration: at the start of every round and then every --calib-every
pairs, `ldbench -K` times the positive control's multiply chain on the
timing core.  The samples go to "calib" (ns per iteration); agg.py uses
them to predict the cost of the posK controls on this machine.

--budget SECS: stop after the current round if the next one would not
finish within SECS of the start (rounds are complete or absent; la.py
copes with fewer rounds).  The JSON is rewritten after every round.
"""
import argparse, json, os, random, shutil, subprocess, sys, time, zlib


def parse_seeds(s):
    res = []
    for part in s.split(','):
        if '-' in part:
            a, b = part.split('-')
            res += list(range(int(a), int(b) + 1))
        else:
            res.append(int(part))
    return res


def parse_cpus(s):
    res = set()
    for part in s.split(','):
        if '-' in part:
            a, b = part.split('-')
            res |= set(range(int(a), int(b) + 1))
        elif part:
            res.add(int(part))
    return res


ap = argparse.ArgumentParser()
ap.add_argument('-o', required=True)
ap.add_argument('-b', required=True)
ap.add_argument('-t', required=True)
ap.add_argument('--seeds', required=True)
ap.add_argument('--files', required=True)
ap.add_argument('-l', default='6')
ap.add_argument('-s', default='0')
ap.add_argument('-n', type=int, default=3)
ap.add_argument('-r', type=int, default=3)
ap.add_argument('--core', default='')
ap.add_argument('--driver-cpus', default='')
ap.add_argument('--aa', action='store_true')
ap.add_argument('--budget', type=float, default=0)
ap.add_argument('--fresh-dir', default='/dev/shm')
ap.add_argument('--no-fresh', action='store_true')
ap.add_argument('--calib-every', type=int, default=40)
ap.add_argument('--calib-iters', type=int, default=2000000)
ap.add_argument('--order', default='perm', choices=['perm', 'rot'])
a = ap.parse_args()
if a.driver_cpus:
    try:  # keep the driver itself off the timing CPU and its sibling
        os.sched_setaffinity(0, parse_cpus(a.driver_cpus))
    except OSError as e:
        print('lt.py: cannot set driver affinity: %s' % e, file=sys.stderr)
FRESH = os.path.join(a.fresh_dir, 'r8lay_run_%d' % os.getpid())

bindir = a.b if a.b.startswith('/') else os.path.join(os.environ['R8LAY_ROOT'] + '-b', a.b)
trees = a.t.split(',')
seeds = parse_seeds(a.seeds)
exe = 'ldbench'
if a.files.startswith('dir:'):
    d = a.files[4:]
    files = sorted(os.path.join(d, f) for f in os.listdir(d))
else:
    files = a.files.split(',')
pin = ['taskset', '-c', a.core] if a.core else []


def binpath(t, s):
    if t == 'AA':
        p = os.path.join(bindir, trees[0], 's%03d' % s, exe + '.aa')
        if not os.path.exists(p):
            shutil.copy(os.path.join(bindir, trees[0], 's%03d' % s, exe), p)
        return p
    return os.path.join(bindir, t, 's%03d' % s, exe)


def run_bin(b, args):
    """Run binary b (a fresh copy of it) with args on the timing core."""
    x = b
    if not a.no_fresh:
        shutil.copyfile(b, FRESH)
        os.chmod(FRESH, 0o755)
        x = FRESH
    out = subprocess.run(pin + [x] + args, capture_output=True, text=True, env=env)
    if not a.no_fresh:
        os.unlink(FRESH)
    if out.returncode:
        sys.exit('%s %s: rc %d %s %s' % (b, ' '.join(args), out.returncode, out.stdout, out.stderr))
    return out.stdout.split()


V = trees + (['AA'] if a.aa else [])
for t in V:
    for s in seeds:
        if not os.path.exists(binpath(t, s)):
            sys.exit('missing ' + binpath(t, s))
rng = random.Random(zlib.crc32(('%s:%s:%s' % (os.path.basename(a.o), a.l, a.s)).encode()))
names = [os.path.basename(f) for f in files]
data = {t: {str(s): {n: [] for n in names} for s in seeds} for t in V}
data_cpu = {t: {str(s): {n: [] for n in names} for s in seeds} for t in V}
data_cyc = {t: {str(s): {n: [] for n in names} for s in seeds} for t in V}
ref = {}
mism = 0
loads = []
calib = []
visits = []
round_secs = []
t0 = time.time()
env = dict(os.environ, LDBENCH_QUIET='1')
timer = ' '.join(run_bin(binpath(trees[0], seeds[0]), ['-T']))
have_cyc = timer.startswith('timer cycles')
print('%s: %s; %d trees x %d seeds x %d files, L%s chunk %s, %d rounds x %d reps, core %s' % (
    a.o, timer, len(V), len(seeds), len(files), a.l, a.s, a.n, a.r, a.core or 'any'), file=sys.stderr, flush=True)


def calibrate(rnd_, i):
    p = run_bin(binpath(trees[0], seeds[0]), ['-K', str(a.calib_iters), '-r', '5'])
    calib.append([rnd_, i, float(p[2]), float(p[3]), float(p[4])])


stopped = ''
for rnd_ in range(a.n):
    tr = time.time()
    pairs = [(s, f) for s in seeds for f in files]
    rng.shuffle(pairs)
    for i, (s, f) in enumerate(pairs):
        if i % a.calib_every == 0:
            calibrate(rnd_, i)
        nm = os.path.basename(f)
        if a.order == 'rot':
            k = (rnd_ + i) % len(V)
            order = V[k:] + V[:k]
            if rnd_ % 2:
                order = order[::-1]
        else:
            order = V[:]
            rng.shuffle(order)
        visits.append([rnd_, s, names.index(nm), [V.index(t) for t in order]])
        for t in order:
            b = binpath(t, s)
            p = run_bin(b, ['-x', '-l', a.l, '-r', str(a.r), '-s', a.s, f])
            if len(p) != 8:
                sys.exit('%s %s: unexpected output %r' % (b, f, p))
            data[t][str(s)][nm].append(float(p[2]))
            data_cpu[t][str(s)][nm].append(float(p[3]))
            if have_cyc:
                data_cyc[t][str(s)][nm].append(float(p[4]))
            h = p[6] + ':' + p[7]
            if nm not in ref:
                ref[nm] = h
            elif ref[nm] != h:
                mism += 1
                print('HASH MISMATCH %s %s: %s vs %s' % (b, nm, h, ref[nm]), file=sys.stderr)
    calibrate(rnd_, len(pairs))
    la = open('/proc/loadavg').read().split()[0]
    loads.append(float(la))
    round_secs.append(time.time() - tr)
    left = a.n - rnd_ - 1
    if a.budget and left and time.time() - t0 + max(round_secs) * 1.05 > a.budget:
        stopped = 'budget: %d of %d rounds' % (rnd_ + 1, a.n)
    res = {'args': vars(a), 'trees': V, 'seeds': seeds, 'files': names, 'data': data,
           'data_cpu': data_cpu, 'loads': loads, 'hash_mismatches': mism, 'hashes': ref,
           'rounds_done': rnd_ + 1, 'secs': time.time() - t0, 'round_secs': round_secs,
           'timer': timer, 'calib': calib, 'stopped': stopped, 'visits': visits}
    if have_cyc:
        res['data_cyc'] = data_cyc
    json.dump(res, open(a.o + '.tmp', 'w'))
    os.replace(a.o + '.tmp', a.o)
    print('%s: round %d/%d load %s %.0fs' % (a.o, rnd_ + 1, a.n, la, time.time() - t0),
          file=sys.stderr, flush=True)
    if stopped:
        print('%s: stopping early (%s)' % (a.o, stopped), file=sys.stderr, flush=True)
        break
print('%s: done, hash mismatches %d' % (a.o, mism), file=sys.stderr)
sys.exit(4 if mism else 0)
