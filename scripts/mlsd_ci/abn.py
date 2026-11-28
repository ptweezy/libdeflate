#!/usr/bin/env python3
# abn.py - interleaved N-build decompression timing with dbench.
#
# Every (file, build) run is its own process.  Within a round, each file is run
# on every build back to back, in an order rotated by round and file (Latin
# square), so slow spells hit all builds alike.  Reported vs REF per file:
#   min% = speedup from each side's best time over all rounds
#   med% = median over rounds of the paired (same round, same file) speedup
# usage: abn.py -o OUT.json -r ROUNDS -n TRIALS [-B BUFSIZE] -ref REF
#               -b NAME,NAME,... -s SUFFIX FILE...
import subprocess, sys, json, os, statistics, math, time

a = sys.argv[1:]
opt = {'-r': '9', '-n': '5', '-B': '', '-ref': 'pr', '-b': '', '-s': '', '-o': ''}
while a and a[0] in opt:
    opt[a[0]] = a[1]
    a = a[2:]
files = a
builds = opt['-b'].split(',')
ref = opt['-ref']
rounds, trials = int(opt['-r']), opt['-n']
here = os.path.dirname(os.path.abspath(__file__))
top = os.path.dirname(here)

def run(b, f):
    cmd = (['taskset', '-c', os.environ.get('BENCH_CPU', '7')] if sys.platform == 'linux' else []) + [os.path.join(top, 'bin', 'dbench_' + b), '-n', os.environ.get('BENCH_TRIALS', trials)]
    if opt['-B']:
        cmd += ['-B', opt['-B']]
    cmd += [os.path.join(top, 'streams', os.path.basename(f) + opt['-s']), f]
    return int(subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True).stdout)

times = {b: {f: [] for f in files} for b in builds}
loads = []
for f in files:                      # warm-up, not recorded
    run(builds[0], f)
t0 = time.time()
for r in range(rounds):
    loads.append(os.getloadavg()[0])
    for fi, f in enumerate(files):
        k = (r + fi) % len(builds)
        order = builds[k:] + builds[:k]
        if r % 2:
            order.reverse()
        for b in order:
            times[b][f].append(run(b, f))
    print('round %d done, load %.2f, %.0fs' % (r + 1, loads[-1], time.time() - t0), file=sys.stderr, flush=True)

json.dump({'opts': opt, 'files': files, 'times': times, 'loads': loads},
          open(opt['-o'], 'w'))

def pct(x):
    return (x - 1) * 100

names = [os.path.basename(f) for f in files]
others = [b for b in builds if b != ref]
print('vs %s: min%% / med%%  (positive = faster than %s); loads %s' %
      (ref, ref, ' '.join('%.1f' % l for l in loads)))
print('%-9s' % 'file' + ''.join('%16s' % b for b in others))
gm = {b: [0.0, 0.0] for b in others}
for f, nm in zip(files, names):
    tr = times[ref][f]
    row = '%-9s' % nm
    for b in others:
        tb = times[b][f]
        mn = min(tr) / min(tb)
        md = statistics.median(x / y for x, y in zip(tr, tb))
        gm[b][0] += math.log(mn)
        gm[b][1] += math.log(md)
        row += '%8.1f/%6.1f ' % (pct(mn), pct(md))
    print(row)
print('%-9s' % 'GEOMEAN' + ''.join('%8.1f/%6.1f ' % (pct(math.exp(gm[b][0] / len(files))),
                                               pct(math.exp(gm[b][1] / len(files))))
                                   for b in others))
print('%-9s' % 'WORST' + ''.join('%8.1f/%6.1f ' % (
    min(pct(min(times[ref][f]) / min(times[b][f])) for f in files),
    min(pct(statistics.median(x / y for x, y in zip(times[ref][f], times[b][f])))
        for f in files)) for b in others))
