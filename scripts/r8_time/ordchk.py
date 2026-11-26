#!/usr/bin/env python3
"""r8_time ordchk.py - does the order of the runs within a visit matter?
(test-only diagnostic for lt.py's data)

usage: ordchk.py DIR... [--metric auto|wall|cpu|cyc]

A visit runs every tree once for one (seed, file) pair, back to back.  For
every run, e = ln(time) - (mean over the visit's runs) - (the tree's mean
of that over all visits), in %.  Per group of shards (as in agg.py) the
script prints the mean of e, with its standard error,
  first       for the first run of a visit vs all later ones
  after-same  for m0 right after its copy AA, or AA right after m0, vs
              the same two trees after any other tree (not first)
  by position for every position in the visit
A process that ran faster right after an identical binary would show as a
negative "after-same".  With lt.py --order perm (the default) such effects
only add noise; with --order rot the copy A/A inherits them.
"""
import math, os, sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import agg, la  # noqa: E402


def mse(v):
    if len(v) < 2:
        return float('nan'), float('nan')
    m = sum(v) / len(v)
    return m, math.sqrt(sum((x - m) ** 2 for x in v) / (len(v) - 1) / len(v))


def main():
    args = sys.argv[1:]
    metric = la.opt(args, '--metric', 'auto')
    shards = agg.find_shards([a for a in args if not a.startswith('--')])
    groups = {}
    for s in shards:
        m = s['meta']
        for (lv, ch), j in s['jobs'].items():
            if 'visits' not in j:
                continue
            key = (m.get('base_sha', '?')[:7], m['arch'], m['cfg'], m.get('fileset', '?'),
                   m.get('ltopts', ''), ch != 0, ch, lv, tuple(j['trees']))
            groups.setdefault(key, []).append(j)
    print('%-58s %7s  %-16s %-16s %s' % ('group', 'visits', 'first', 'after-same', 'by position'))
    for key in sorted(groups):
        base, arch, cfg, fileset, lto, _, ch, lv, V = key
        jobs = groups[key]
        gm = metric
        if metric == 'auto':
            gm = 'cyc' if all('data_cyc' in j for j in jobs) else 'wall'
        rows = []
        for j in jobs:
            D = j[la.METRICS[gm]]
            for rnd_, seed, fi, order in j['visits']:
                f = j['files'][fi]
                try:
                    v = [math.log(D[t][str(seed)][f][rnd_]) for t in V]
                except (IndexError, KeyError):
                    continue
                mv = sum(v) / len(v)
                rows.append((order, [x - mv for x in v]))
        if not rows:
            continue
        n = len(V)
        tm = [sum(r[1][t] for r in rows) / len(rows) for t in range(n)]
        pos = [[] for _ in range(n)]
        first, later, same, other = [], [], [], []
        im0 = V.index('m0') if 'm0' in V else -1
        iaa = V.index('AA') if 'AA' in V else -1
        for order, res in rows:
            for p, t in enumerate(order):
                e = (res[t] - tm[t]) * 100
                pos[p].append(e)
                (later if p else first).append(e)
                if p and t in (im0, iaa) and iaa >= 0:
                    (same if order[p - 1] in (im0, iaa) else other).append(e)
        f, fse = mse(first)
        l, lse = mse(later)
        s, sse = mse(same)
        o, ose = mse(other)
        name = '%s %s %s L%d %s%s (%s)' % (agg.cfgname(cfg), arch, fileset, lv, 'whole' if ch == 0 else ch,
                                         (' [%s]' % lto) if lto else '', gm)
        print('%-58s %7d  %+.3f (%.3f)   %+.3f (%.3f)   %s' % (
            name[:58], len(rows), f - l, math.hypot(fse, lse), s - o, math.hypot(sse, ose),
            ' '.join('%+.3f' % mse(p)[0] for p in pos)))


if __name__ == '__main__':
    main()
