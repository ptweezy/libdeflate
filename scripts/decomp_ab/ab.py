#!/usr/bin/env python3
# decomp_ab ab.py - test-only: interleaved A/B driver for bench.c.
#
# Each round runs A and B back to back on ONE file at a time (ABBA order
# alternating by round and file), so a burst of machine noise hits both sides.
# Reported per (file, level):
#   min%  = speedup from the minimum time of each side over all rounds
#   med%  = median over rounds of the paired (same-round) speedup
# A large gap between the two means the measurement was noisy.
#
# With BENCH_PMU=1 in the environment, the bench harness reports user-mode CPU
# cycles instead of nanoseconds (the "MB/s" columns are then bytes per 1000
# cycles) and instruction counts, whose change is printed as ins%.
#
# usage: ab.py [-r ROUNDS] [-cpu N] [-q] A_EXE B_EXE -- bench-args... FILES...
import subprocess, sys, math, os, shutil, statistics

args = sys.argv[1:]
rounds = 5
cpu = '5'
quiet = False
while args and args[0] in ('-r', '-cpu', '-q'):
    if args[0] == '-r':
        rounds = int(args[1]); args = args[2:]
    elif args[0] == '-cpu':
        cpu = args[1]; args = args[2:]
    else:
        quiet = True; args = args[1:]
A, B = os.path.abspath(args[0]), os.path.abspath(args[1])
rest = args[3:] if args[2] == '--' else args[2:]
files = [x for x in rest if os.path.isfile(x)]
opts = [x for x in rest if not os.path.isfile(x)]
pmu = bool(os.environ.get('BENCH_PMU'))

def run(exe, f):
    prefix = ['taskset', '-c', cpu] if shutil.which('taskset') else []
    out = subprocess.run(prefix + [exe] + opts + [f], check=True,
                         stdout=subprocess.PIPE, text=True).stdout
    res = {}
    for line in out.splitlines():
        fl = line.split()
        name, level, insz, outsz, h, ct, dt = fl[:7]
        ci, di = (int(fl[7]), int(fl[8])) if len(fl) >= 9 else (0, 0)
        res[(name, int(level))] = (int(insz), int(outsz), h, int(ct), int(dt), ci, di)
    return res

info = {'A': {}, 'B': {}}
times = {'A': {}, 'B': {}}   # key -> list of (ct, dt) per round
for r in range(rounds):
    for fi, f in enumerate(files):
        order = (('A', A), ('B', B)) if (r + fi) % 2 == 0 else (('B', B), ('A', A))
        for tag, exe in order:
            for k, v in run(exe, f).items():
                info[tag][k] = v
                times[tag].setdefault(k, []).append((v[3], v[4]))

keys = list(times['A'].keys())
levels = sorted({k[1] for k in keys})
names = []
for k in keys:
    if k[0] not in names:
        names.append(k[0])

def pct(x):
    return (x - 1) * 100

unit = 'B/kcyc' if pmu else 'MB/s'
scale = 1e3 if pmu else 1e3  # bytes per 1000 cycles, or MB/s (bytes/ns * 1e3)
hdr = '%-10s %3s %10s %8s | %8s %8s %7s %7s %7s | %8s %8s %7s %7s %7s' % (
    'file', 'lvl', 'sizeA', 'dsize%', 'cA ' + unit, 'cB', 'c min%', 'c med%', 'c ins%',
    'dA ' + unit, 'dB', 'd min%', 'd med%', 'd ins%')
print(hdr)
for lvl in levels:
    acc = [0.0] * 6
    n = 0
    toa = tob = 0
    for name in names:
        k = (name, lvl)
        ia, ib = info['A'][k], info['B'][k]
        ta, tb = times['A'][k], times['B'][k]
        row = []
        for idx in (0, 1):
            ma = min(t[idx] for t in ta)
            mb = min(t[idx] for t in tb)
            if ma == 0 or mb == 0:
                row += [0, 0, 1.0, 1.0, 1.0]
                continue
            paired = statistics.median(x[idx] / y[idx] for x, y in zip(ta, tb))
            ins = (ia[5 + idx] / ib[5 + idx]) if ia[5 + idx] and ib[5 + idx] else 1.0
            row += [ia[0] / ma * scale, ia[0] / mb * scale, ma / mb, paired, 1 / ins]
        same = '=' if ia[2] == ib[2] else '!'
        if not quiet:
            print('%-10s %3d %10d %+7.3f%s | %8.2f %8.2f %+6.1f%% %+6.1f%% %+6.1f%% | %8.1f %8.1f %+6.1f%% %+6.1f%% %+6.1f%%' % (
                name, lvl, ia[1], pct(ib[1] / ia[1]), same,
                row[0], row[1], pct(row[2]), pct(row[3]), pct(row[4]),
                row[5], row[6], pct(row[7]), pct(row[8]), pct(row[9])))
        for j, v in enumerate((row[2], row[3], row[4], row[7], row[8], row[9])):
            acc[j] += math.log(v)
        n += 1
        toa += ia[1]; tob += ib[1]
    g = [pct(math.exp(x / n)) for x in acc]
    print('%-10s %3d %10d %+7.3f%% | %8s %8s %+6.1f%% %+6.1f%% %+6.1f%% | %8s %8s %+6.1f%% %+6.1f%% %+6.1f%%' % (
        'GEOMEAN', lvl, toa, pct(tob / toa), '', '', g[0], g[1], g[2], '', '', g[3], g[4], g[5]))
    if not quiet:
        print()
