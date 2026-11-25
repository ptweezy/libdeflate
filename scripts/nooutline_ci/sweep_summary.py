#!/usr/bin/env python3
"""nooutline_ci sweep_summary.py - test-only: table of a sweep.sh run.

usage: sweep_summary.py OUTDIR...   (each with meta.txt and res.txt)

For each decompression level, one row per library (the PR, then each
placement k0..k112), all against master: the main loop head's offset within
a 128-byte line, the geometric mean over files of the speedup from medians
over rounds ("med") and from minimums over rounds ("min"), nci, xml, and the
worst and best files (median-based).  Several OUTDIRs from the same kind of
machine are combined by taking each file's median over them.  Positive =
faster than master.
"""
import math
import statistics
import sys


def parse(d):
    meta = {}
    for line in open(d + '/meta.txt'):
        k, _, v = line.partition(':')
        meta.setdefault(k.strip(), v.strip())
    names = dict(x.split('=') for x in meta['libs'].split())
    names = {int(k): v for k, v in names.items()}
    h = meta.get('heads', '').split()
    heads = dict(zip(h[0::2], h[1::2]))
    res = {}
    cur = None
    for line in open(d + '/res.txt'):
        line = line.strip()
        if line.startswith('==='):
            w = line.split()
            cur = res.setdefault(int(w[3]), {}).setdefault(int(w[7]), {})
            continue
        if line:
            head = line.split('|')[0].split()
            cur.setdefault(head[0], []).append(float(head[2]))
    return meta, names, heads, res


def gm(xs):
    xs = list(xs)
    return math.exp(sum(math.log(1 + x) for x in xs) / len(xs)) - 1


def pct(x):
    return '%+6.2f%%' % (100 * x)


runs = [parse(d) for d in sys.argv[1:]]
meta, names, heads, _ = runs[0]
print('# Placement sweep: %s, %s' % (meta.get('cpu', '?'), meta.get('cc', '?')))
print('runs: %s' % ' '.join(sys.argv[1:]))
print('libs: %s; master loop head %s, PR loop head %s' % (meta['libs'], heads.get('master'), heads.get('pr')))
for level in sorted({lv for _, _, _, r in runs for lv in r}):
    print('\n## Decompression, L%d stream (vs master)\n' % level)
    print('| build | loop head | med | min | nci | xml | worst file | best file |')
    print('|---|---|---|---|---|---|---|---|')
    for lib in sorted(names):
        if lib == 0:
            continue
        per_file_med, per_file_min = {}, {}
        for _, _, _, r in runs:
            t = r[level]
            for f in t[0]:
                a, b = t[0][f], t[lib][f]
                per_file_med.setdefault(f, []).append(statistics.median(a) / statistics.median(b) - 1)
                per_file_min.setdefault(f, []).append(min(a) / min(b) - 1)
        med = {f: statistics.median(v) for f, v in per_file_med.items()}
        mn = {f: statistics.median(v) for f, v in per_file_min.items()}
        worst = min(med, key=med.get)
        best = max(med, key=med.get)
        name = names[lib]
        label = 'A/A (copy of master)' if name == 'copy' else name
        print('| %s | %s | %s | %s | %s | %s | %s %s | %s %s |' % (
            label, heads.get(name, '-'), pct(gm(med.values())), pct(gm(mn.values())),
            pct(med.get('nci', 0)), pct(med.get('xml', 0)), worst, pct(med[worst]), best, pct(med[best])))
