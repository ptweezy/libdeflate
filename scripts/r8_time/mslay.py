#!/usr/bin/env python3
"""mslay.py DIR BASE CAND PADS  (test-only): layout-averaged MSVC comparison.

DIR holds JOB/res.tsv and JOB/cpu.txt from msab.py, with the exes BASEpP and
CANDpP for each P in PADS (comma-separated, e.g. 0,16,32,48; 0 = BASE and
CAND themselves): the same two libraries, linked with their code moved.  Per arch,
set and runner CPU model, and per file, each exe's time is its minimum over
the shards.  For each cell, the table gives the mean over files of:

  CAND vs BASE  exp(mean_P ln t_BASE,P - mean_P ln t_CAND,P) - 1, in percent
                (+ = CAND faster), i.e. averaged over the code positions
  per position  the same at each P alone
  BASE range    max_P - min_P of ln t_BASE,P: how much moving BASE's code
                alone changes its speed (likewise CAND range)
"""
import glob, math, os, statistics, sys

root, base, cand = sys.argv[1:4]
pads = [int(p) for p in sys.argv[4].split(',')]
def exe(n, p):
    return n if p == 0 else '%sp%d' % (n, p)
t, files, cfgs, jobs = {}, {}, {}, {}
for path in sorted(glob.glob(os.path.join(root, '*', 'res.tsv'))):
    job = os.path.basename(os.path.dirname(path))
    cpu = os.path.join(os.path.dirname(path), 'cpu.txt')
    model = ' '.join(open(cpu).read().split()[:-1]) if os.path.exists(cpu) else '?'
    grp = '-'.join(job.split('-')[-3:-1]) + ' on ' + model
    jobs.setdefault(grp, []).append(job)
    for line in open(path):
        lvl, chunk, f, name, tt, _, same = line.rstrip('\n').split('\t')
        assert same == 'same', line
        cfg = 'L%s:%s' % (lvl, chunk)
        k = (grp, cfg, f, name)
        t[k] = min(t.get(k, 1e30), float(tt))
        files.setdefault(grp, {}).setdefault(f, None)
        cfgs.setdefault(grp, {}).setdefault(cfg, None)
pct = lambda x: (math.exp(x) - 1) * 100
for grp in sorted(files):
    print('## MSVC %s\n\nshards: %s\n' % (grp, ', '.join(jobs[grp])))
    print('| cell | %s vs %s | %s | %s range | %s range |' % (
        cand, base, ' | '.join('pad %d' % p for p in pads), base, cand))
    print('|---|---:|' + '---:|' * len(pads) + '---:|---:|')
    for cfg in cfgs[grp]:
        avg, per, rb, rc = [], {p: [] for p in pads}, [], []
        for f in files[grp]:
            lb = [math.log(t[(grp, cfg, f, exe(base, p))]) for p in pads]
            lc = [math.log(t[(grp, cfg, f, exe(cand, p))]) for p in pads]
            avg.append(statistics.mean(lb) - statistics.mean(lc))
            for p, x, y in zip(pads, lb, lc):
                per[p].append(x - y)
            rb.append(max(lb) - min(lb))
            rc.append(max(lc) - min(lc))
        print('| %s | **%+.2f** | %s | %.2f | %.2f |' % (
            cfg, pct(statistics.mean(avg)),
            ' | '.join('%+.2f' % pct(statistics.mean(per[p])) for p in pads),
            pct(statistics.mean(rb)), pct(statistics.mean(rc))))
    print()
