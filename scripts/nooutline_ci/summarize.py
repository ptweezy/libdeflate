#!/usr/bin/env python3
"""nooutline_ci summarize.py - test-only: one Markdown report from all jobs.

usage: summarize.py DIR
       summarize.py --one RES_TXT TAG MODE LEVEL   (one line, for the live log)

DIR holds one subdirectory per downloaded artifact: time-* (meta.txt, res.txt
from time.sh) and probe-* (probe.txt from probe.sh).

In res.txt every "=== TAG MODE LEVEL round R lib J" section is one abp process
that timed one library (0 = base, 1 = test, 2 = copy of base, 3 = extra) on
one or more files; its number for a file is the minimum over its reps (ns per
input byte).  Within a job, a file's speedup is median(base) / median(lib) - 1
over the rounds, and "A/A" is median(base) / median(copy) - 1 (a noise floor).
"paired" is the other estimator: the median over rounds of base / lib from the
same round.  Jobs with the same image and compiler are combined by taking each
file's median over the jobs; a level's figure is the geometric mean over the
files.
"""
import math
import os
import statistics
import sys


def parse_meta(path):
    meta = {}
    with open(path) as f:
        for line in f:
            k, _, v = line.partition(':')
            meta.setdefault(k.strip(), v.strip())
    # Older runs called the trees "master" and "pr".
    if 'test' not in meta and 'pr' in meta:
        meta['test'] = meta['pr']
    if 'base' not in meta and 'master' in meta:
        meta['base'] = meta['master']
    return meta


def parse_res(path):
    """-> {(tag, mode, level): {'t': {lib: {file: [time per round]}}, 'ratio': {file: str}}}"""
    res = {}
    cur = lib = None
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith('==='):
                w = line.split()
                sec = res.setdefault((w[1], w[2], int(w[3])), {'t': {}, 'ratio': {}})
                lib = int(w[7])
                cur = sec['t'].setdefault(lib, {})
                continue
            head = line.split('|')[0].split()
            cur.setdefault(head[0], []).append(float(head[2]))
            sec['ratio'][head[0]] = head[1]
    return res


def gmean(xs):
    xs = list(xs)
    return math.exp(sum(math.log(1 + x) for x in xs) / len(xs)) - 1


def pct(x):
    return '%+.2f%%' % (100 * x)


def job_stats(sec, lib=1):
    """file -> (speedup of lib over base, A/A speedup or None, paired speedup)"""
    t = sec['t']
    out = {}
    for f in t.get(0, {}):
        if f not in t.get(lib, {}):
            continue
        a, b = t[0][f], t[lib][f]
        sp = statistics.median(a) / statistics.median(b) - 1
        aa = None
        if f in t.get(2, {}):
            aa = statistics.median(a) / statistics.median(t[2][f]) - 1
        paired = statistics.median(x / y for x, y in zip(a, b)) - 1
        out[f] = (sp, aa, paired)
    return out


def one(path, tag, mode, level):
    sec = parse_res(path)[(tag, mode, int(level))]
    parts = []
    for lib, name in ((1, 'test'), (3, 'extra')):
        if lib not in sec['t']:
            continue
        st = job_stats(sec, lib)
        sp = {f: v[0] for f, v in st.items()}
        worst = min(sp, key=sp.get)
        best = max(sp, key=sp.get)
        parts.append('%s %s (paired %s, worst %s %s, best %s %s)' % (
            name, pct(gmean(sp.values())), pct(gmean(v[2] for v in st.values())),
            worst, pct(sp[worst]), best, pct(sp[best])))
    aas = [v[1] for v in job_stats(sec, 1).values() if v[1] is not None]
    parts.append('A/A %s' % (pct(gmean(aas)) if aas else '-'))
    print('  '.join(parts))


LABEL = {'c': 'Compression L%d', 'd': 'Decompression, L%d stream'}


def report(top):
    jobs = []
    probes = []
    for d in sorted(os.listdir(top)):
        p = os.path.join(top, d)
        if os.path.isfile(os.path.join(p, 'probe.txt')):
            with open(os.path.join(p, 'probe.txt')) as f:
                probes.append((d, f.read()))
        if os.path.isfile(os.path.join(p, 'meta.txt')):
            meta = parse_meta(os.path.join(p, 'meta.txt'))
            rp = os.path.join(p, 'res.txt')
            res = parse_res(rp) if os.path.isfile(rp) else {}
            jobs.append((d, meta, res))

    groups = {}
    for d, meta, res in jobs:
        key = (meta.get('image', '?'), meta.get('cc', '?'), meta.get('test', '?'), meta.get('extra', ''))
        groups.setdefault(key, []).append((d, meta, res))

    out = ['# Timing report\n']
    for (image, cc, test, extra), members in groups.items():
        meta0 = members[0][1]
        out.append('## %s, %s\n' % (image, meta0.get('xcode', cc).split(' at ')[0] or cc))
        out.append('- compiler: %s' % cc)
        out.append('- CPU: %s; OS: %s' % ('; '.join(sorted({m.get('cpu', '?') for _, m, _ in members})),
                                          meta0.get('os', '?')))
        out.append('- base: %s' % meta0.get('base', '?'))
        out.append('- test: %s' % test)
        if extra:
            out.append('- extra: %s' % extra)
        for key in ('flag', 'outlined', 'text', 'fastloop', 'identity', 'aligned'):
            if key in meta0:
                out.append('- %s: %s' % (key, meta0[key]))
        if any('identical' in m for _, m, _ in members):
            out.append('- **The test tree\'s code is identical to the base tree\'s with this compiler; nothing timed.**\n')
            continue
        out.append('- timing jobs: %d (%d finished: %s)\n' % (
            len(members), sum('ALL DONE' in m for _, m, _ in members), ' '.join(d for d, _, _ in members)))

        keys = sorted({k for _, _, r in members for k in r},
                      key=lambda k: (k[0] != 'main', k[1] != 'c', k[2]))
        out.append('| | vs base | per job | worst file | best file | A/A control | A/A worst | paired |')
        out.append('|---|---|---|---|---|---|---|---|')
        perfile = {}
        for k in keys:
            for lib, name in ((1, 'test'), (3, 'extra')):
                per_file = {}
                job_g = []
                for _, _, res in members:
                    if k not in res or lib not in res[k]['t']:
                        continue
                    st = job_stats(res[k], lib)
                    if not st:
                        continue
                    for f, v in st.items():
                        per_file.setdefault(f, []).append(v)
                    job_g.append(gmean(v[0] for v in st.values()))
                if not per_file:
                    continue
                med = {f: statistics.median(x[0] for x in v) for f, v in per_file.items()}
                med_aa = {f: statistics.median(x[1] for x in v) for f, v in per_file.items()
                          if all(x[1] is not None for x in v)}
                med_pd = {f: statistics.median(x[2] for x in v) for f, v in per_file.items()}
                perfile[(k, lib)] = (per_file, med, med_aa)
                worst = min(med, key=med.get)
                best = max(med, key=med.get)
                label = LABEL[k[1]] % k[2] + ('' if k[0] == 'main' else ' (%s)' % k[0])
                if lib == 3:
                    label += ' [extra]'
                aa = aaw = '-'
                if med_aa and lib == 1:
                    w = max(med_aa, key=lambda f: abs(med_aa[f]))
                    aa, aaw = pct(gmean(med_aa.values())), '%s %s' % (w, pct(med_aa[w]))
                out.append('| %s | **%s** | %s | %s %s | %s %s | %s | %s | %s |' % (
                    label, pct(gmean(med.values())), ' '.join(pct(g) for g in job_g),
                    worst, pct(med[worst]), best, pct(med[best]), aa, aaw,
                    pct(gmean(med_pd.values()))))
        out.append('')
        for k in (('main', 'c', 6), ('main', 'd', 6)):
            for lib, name in ((1, 'test'), (3, 'extra')):
                if (k, lib) not in perfile:
                    continue
                per_file, med, med_aa = perfile[(k, lib)]
                ratio = {}
                for _, _, res in members:
                    ratio.update(res.get(k, {}).get('ratio', {}))
                out.append('<details><summary>Per file, %s, %s vs base (median over jobs; per job after it)</summary>\n'
                           % (LABEL[k[1]] % k[2], name))
                out.append('| file | ratio | vs base | per job | A/A |')
                out.append('|---|---|---|---|---|')
                for f in sorted(per_file):
                    out.append('| %s | %s | %s | %s | %s |' % (
                        f, ratio.get(f, ''), pct(med[f]), ' '.join(pct(x[0]) for x in per_file[f]),
                        pct(med_aa[f]) if f in med_aa else '-'))
                out.append('\n</details>\n')

    for d, text in probes:
        out.append('## Probe: %s\n' % d)
        out.append('```')
        out.append(text.rstrip())
        out.append('```\n')
    print('\n'.join(out))


if __name__ == '__main__':
    if sys.argv[1] == '--one':
        one(*sys.argv[2:6])
    else:
        report(sys.argv[1])
