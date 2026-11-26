#!/usr/bin/env python3
"""r8_time la.py - analyse lt.py JSON files (randomized-layout timing);
test-only, the CI port of handoff/tools/layout-harness/la.py.

usage: la.py [-f] [-q] [--metric auto|wall|cpu|cyc] [--est min|pair]
             OUT.json [BASE:CAND ...]

Per seed s, file f and tree t: x = min over rounds (--est min, the
default).  For BASE:CAND, r(s,f) = ln(x_base/x_cand) (> 0: CAND faster).
With --est pair, r(s,f) is instead the mean over rounds of
ln(x_base,k/x_cand,k), i.e. each round's back-to-back runs are compared
with each other only (robust to a machine whose speed drifts between
rounds, at the price of not discarding slow rounds).  Per seed, the
geomean over files g(s) = mean_f r(s,f).  The effect is the mean over
seeds, with a 95% confidence interval from the spread across seeds
(Student t, S-1 df), so the CI covers the layout variance, the per-binary
state and the run-to-run noise together.  Printed as percentages: effect
[CI low, CI high] +-half.

--metric picks the clock: wall (CLOCK_MONOTONIC ns), cpu (thread CPU ns),
cyc (user-mode cycles; only if the machine had a PMU), or auto (the
default): cyc if the file has cycle counts, else wall.

Default comparisons: every tree vs the first, each tree vs the previous one
(only among the trees named m0, f1, f2, ...; control trees are compared
with the first tree only), and AA vs the first tree (the copy A/A).  Also
printed: the cross-seed A/A (the first tree's seeds split in two halves,
seed i vs seed i+S/2: the same code in independent layouts), the spread of
g(s), and the seeds needed for a +-0.1% / +-0.2% half-width at that spread.

Verdict per comparison: REGRESS if the geomean CI lies entirely below 0 or
any file's CI lies entirely below -0.3%; GAIN if the geomean CI lies
entirely above 0 and no file regresses; else "~0" (within resolution).
-f prints the per-file table; -q only the one-line summaries.
"""
import json, math, re, sys

T975 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306,
        9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179, 13: 2.160, 14: 2.145, 15: 2.131,
        16: 2.120, 17: 2.110, 18: 2.101, 19: 2.093, 20: 2.086, 21: 2.080, 22: 2.074,
        23: 2.069, 24: 2.064, 25: 2.060, 26: 2.056, 27: 2.052, 28: 2.048, 29: 2.045,
        30: 2.042, 40: 2.021, 50: 2.009, 60: 2.000, 80: 1.990, 100: 1.984, 120: 1.980}
METRICS = {'wall': 'data', 'cpu': 'data_cpu', 'cyc': 'data_cyc'}


def tq(df):
    if df in T975:
        return T975[df]
    ks = sorted(T975)
    if df > ks[-1]:
        return 1.96
    lo = max(k for k in ks if k < df)
    hi = min(k for k in ks if k > df)
    return T975[lo] + (T975[hi] - T975[lo]) * (df - lo) / (hi - lo)


def mci(xs):
    n = len(xs)
    m = sum(xs) / n
    if n < 2:
        return m, float('nan'), float('nan')
    sd = math.sqrt(sum((x - m) ** 2 for x in xs) / (n - 1))
    return m, sd, tq(n - 1) * sd / math.sqrt(n)


def pc(x):
    return (math.exp(x) - 1) * 100


def need(sd, hw):
    if not sd == sd:
        return -1
    return math.ceil((1.96 * sd / (hw / 100)) ** 2)


def load(fn, metric='wall'):
    """Returns (json, x, seeds, files): x[tree][seed][file] = the list of
    per-round values, for the seeds that have every file."""
    d = json.load(open(fn)) if isinstance(fn, str) else fn
    if metric == 'auto':
        metric = 'cyc' if 'data_cyc' in d else 'wall'
    key = METRICS[metric]
    if key not in d:
        sys.exit('%s: no %s data' % (fn if isinstance(fn, str) else 'input', metric))
    D = d[key]
    seeds = [str(s) for s in d['seeds']]
    files = d['files']
    x = {}
    for t in D:
        x[t] = {}
        for s in seeds:
            if s in D[t] and all(D[t][s].get(f) for f in files):
                x[t][s] = {f: D[t][s][f] for f in files}
    return d, x, seeds, files


def ratio(xb, xc, est):
    """ln(base/cand) for one (seed, file) from the two lists of rounds."""
    if est == 'pair':
        n = min(len(xb), len(xc))
        return sum(math.log(xb[k] / xc[k]) for k in range(n)) / n
    return math.log(min(xb) / min(xc))


def effects(x, files, b, c, pairs, est='min'):
    """Per-seed geomeans and per-file ratios for BASE b vs CAND c over the
    (base seed, cand seed) pairs that both have data."""
    gs = []
    rf = {f: [] for f in files}
    seen = []
    for sb, sc in pairs:
        if sb not in x[b] or sc not in x[c]:
            continue
        rs = {f: ratio(x[b][sb][f], x[c][sc][f], est) for f in files}
        for f in files:
            rf[f].append(rs[f])
        gs.append(sum(rs.values()) / len(files))
        seen.append(sb if sb == sc else sb + '/' + sc)
    return gs, rf, seen


def compare(x, files, b, c, pairs, perfile, label, quiet, est='min'):
    gs, rf, pairs_seen = effects(x, files, b, c, pairs, est)
    if len(gs) == 1:
        # one layout (e.g. the plain build): point estimate only
        print('  %-14s %+7.3f%% (single layout, no CI; files neg %d/%d, min %+.3f max %+.3f)' % (
            label, pc(gs[0]), sum(1 for f in files if rf[f][0] < 0), len(files),
            pc(min(rf[f][0] for f in files)), pc(max(rf[f][0] for f in files))))
        if perfile and not quiet:
            print('      per file: ' + ' '.join('%s %+.2f' % (f[:8], pc(rf[f][0])) for f in files))
        return
    if len(gs) < 2:
        print('  %-14s too few seeds' % label)
        return
    m, sd, hw = mci(gs)
    fstats = {f: mci(rf[f]) for f in files}
    freg = [f for f in files if pc(fstats[f][0] + fstats[f][2]) < -0.3]
    neg = sum(1 for f in files if fstats[f][0] < 0)
    if m + hw < 0 or freg:
        v = 'REGRESS'
    elif m - hw > 0:
        v = 'GAIN'
    else:
        v = '~0'
    worst = min(files, key=lambda f: fstats[f][0])
    print('  %-14s %+7.3f%% [%+.3f, %+.3f] +-%.3f  S=%d sd %.3f  files neg %d/%d, worst %s %+.3f +-%.3f%s  %s  (seeds for +-0.1/0.2: %d/%d)' % (
        label, pc(m), pc(m - hw), pc(m + hw), hw * 100, len(gs), sd * 100, neg, len(files),
        worst[:10], pc(fstats[worst][0]), fstats[worst][2] * 100,
        (' file-regress: ' + ','.join(f[:10] for f in freg)) if freg else '', v,
        need(sd, 0.1), need(sd, 0.2)))
    if perfile and not quiet:
        print('      per file: ' + ' '.join('%s %+.2f+-%.2f' % (f[:8], pc(fstats[f][0]), fstats[f][2] * 100)
                                          for f in files))
    # outlier seeds (|g - median| > 6 robust sd, robust sd = 1.4826 MAD):
    # usually a binary with a bad physical placement (see lt.py, fresh copies)
    srt = sorted(gs)
    med = srt[len(srt) // 2]
    mad = sorted(abs(g - med) for g in gs)[len(gs) // 2] * 1.4826
    out = [i for i, g in enumerate(gs) if mad > 0 and abs(g - med) > 6 * mad and abs(g - med) > 0.003]
    if out:
        rest = [g for i, g in enumerate(gs) if i not in out]
        m2, sd2, hw2 = mci(rest) if len(rest) > 1 else (float('nan'),) * 3
        print('      OUTLIER seeds %s (%s); without them %+.3f +-%.3f sd %.3f' % (
            ','.join(pairs_seen[i] for i in out), ' '.join('%+.2f' % pc(gs[i]) for i in out),
            pc(m2), hw2 * 100, sd2 * 100))
    return m, hw, v


def default_cmps(trees):
    real = [t for t in trees if t != 'AA']
    series = [t for t in real if t == real[0] or re.match(r'^[a-z]\d+$', t)]
    cmps = [(real[0], t) for t in real[1:]]
    cmps += [(series[i - 1], series[i]) for i in range(2, len(series))]
    if 'AA' in trees:
        cmps.append((real[0], 'AA'))
    return cmps


def opt(args, name, default):
    if name in args:
        i = args.index(name)
        v = args[i + 1]
        del args[i:i + 2]
        return v
    return default


def main():
    args = sys.argv[1:]
    metric = opt(args, '--metric', 'auto')
    est = opt(args, '--est', 'min')
    perfile = '-f' in args
    quiet = '-q' in args
    args = [a for a in args if a not in ('-f', '-q')]
    if not args:
        sys.exit(__doc__)
    fn = args[0]
    d, x, seeds, files = load(fn, metric)
    if metric == 'auto':
        metric = 'cyc' if 'data_cyc' in d else 'wall'
    a = d['args']
    trees = [t for t in d['trees']]
    print('== %s  %s L%s s%s  trees %s  seeds %d  rounds %d/%d reps %s  files %d  load max %.2f  hash mism %d  %.0fs  [%s, %s; %s]' % (
        fn.split('/')[-1], a['b'].split('/')[-1], a['l'], a['s'], ','.join(trees), len(seeds),
        d.get('rounds_done', a['n']), a['n'], a['r'], len(files), max(d['loads']) if d['loads'] else -1,
        d['hash_mismatches'], d.get('secs', 0), metric, est, d.get('timer', '?')))
    cmps = [tuple(c.split(':')) for c in args[1:]] or default_cmps(trees)
    same = [(s, s) for s in seeds]
    for b, c in cmps:
        compare(x, files, b, c, same, perfile, '%s/%s' % (c, b), quiet, est)
    # cross-seed A/A: same code, independent layouts
    t0 = [t for t in trees if t != 'AA'][0]
    ss = [s for s in seeds if s in x[t0]]
    h = len(ss) // 2
    if h >= 2:
        compare(x, files, t0, t0, list(zip(ss[:h], ss[h:2 * h])), perfile, 'xA/A ' + t0, quiet, est)
    if not quiet and ss:
        # layout spread of the first tree: per-seed geomean vs its mean
        gm = {s: sum(math.log(min(x[t0][s][f])) for f in files) / len(files) for s in ss}
        mm = sum(gm.values()) / len(gm)
        dev = sorted((gm[s] - mm) * 100 for s in ss)
        print('      %s layout spread (geomean over files, %% vs mean): min %+.2f median %+.2f max %+.2f sd %.2f' % (
            t0, dev[0], dev[len(dev) // 2], dev[-1], mci([v / 100 for v in dev])[1] * 100))


if __name__ == '__main__':
    main()
