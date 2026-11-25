#!/usr/bin/env python3
"""nooutline_ci summarize.py - test-only: one Markdown report from all jobs.

usage: summarize.py DIR
       summarize.py --one RES_TXT TAG MODE LEVEL   (one line, for the live log)

DIR holds one subdirectory per downloaded artifact: time-* (meta.txt, res.txt
from time.sh) and probe-* (probe.txt from probe.sh).

In res.txt every "=== TAG MODE LEVEL round R lib J" section is one abp process
that timed one library (0 = master, 1 = PR, 2 = copy of master) on every file;
its number for a file is the minimum over its reps (ns per input byte).
Within a job, a file's speedup is median(master) / median(PR) - 1 over the
rounds, and "A/A" is median(master) / median(copy) - 1 (a noise floor).
"paired" is the other estimator: the median over rounds of master / PR from
the same round.  Jobs with the same image and compiler are combined by taking
each file's median over the jobs; a level's figure is the geometric mean over
the files.
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


def job_stats(sec):
    """file -> (pr speedup, A/A speedup or None, paired pr speedup)"""
    t = sec['t']
    out = {}
    for f in t.get(0, {}):
        if f not in t.get(1, {}):
            continue
        a, b = t[0][f], t[1][f]
        pr = statistics.median(a) / statistics.median(b) - 1
        aa = None
        if f in t.get(2, {}):
            aa = statistics.median(a) / statistics.median(t[2][f]) - 1
        paired = statistics.median(x / y for x, y in zip(a, b)) - 1
        out[f] = (pr, aa, paired)
    return out


def one(path, tag, mode, level):
    res = parse_res(path)
    st = job_stats(res[(tag, mode, int(level))])
    pr = {f: v[0] for f, v in st.items()}
    aas = [v[1] for v in st.values() if v[1] is not None]
    worst = min(pr, key=pr.get)
    best = max(pr, key=pr.get)
    print('PR %s  A/A %s  paired %s  worst %s %s  best %s %s' % (
        pct(gmean(pr.values())), pct(gmean(aas)) if aas else '-',
        pct(gmean(v[2] for v in st.values())), worst, pct(pr[worst]), best, pct(pr[best])))


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
        groups.setdefault((meta.get('image', '?'), meta.get('cc', '?')), []).append((d, meta, res))

    out = ['# PR 487 (-mno-outline on Apple) on GitHub-hosted Apple silicon runners\n']
    for (image, cc), members in groups.items():
        meta0 = members[0][1]
        out.append('## %s, %s\n' % (image, meta0.get('xcode', '?').split(' at ')[0]))
        out.append('- compiler: %s' % cc)
        out.append('- CPU: %s; OS: %s' % ('; '.join(sorted({m.get('cpu', '?') for _, m, _ in members})),
                                          meta0.get('os', '?')))
        out.append('- %s' % meta0.get('flag', '?'))
        out.append('- OUTLINED_FUNCTIONs in libdeflate.dylib: %s' % meta0.get('outlined', '?'))
        t = meta0.get('text', '').split()
        if len(t) == 4:
            a, b = int(t[1]), int(t[3])
            out.append('- __text: master %d, PR %d (%+d bytes, %+.1f%%)' % (a, b, b - a, 100.0 * (b - a) / a))
        if any('identical' in m for _, m, _ in members):
            out.append('- **The PR\'s arm64 code is identical to master\'s with this compiler; nothing timed.**\n')
            continue
        for key in ('identity', 'aligned'):
            if key in meta0:
                out.append('- %s: %s' % (key, meta0[key]))
        out.append('- timing jobs: %d (%d finished: %s)\n' % (
            len(members), sum('ALL DONE' in m for _, m, _ in members), ' '.join(d for d, _, _ in members)))

        keys = sorted({k for _, _, r in members for k in r},
                      key=lambda k: (k[0] != 'main', k[1] != 'c', k[2]))
        out.append('| | PR vs master | per job | worst file | best file | A/A control | A/A worst | paired |')
        out.append('|---|---|---|---|---|---|---|---|')
        perfile = {}
        for k in keys:
            per_file = {}
            job_g = []
            for _, _, res in members:
                if k not in res:
                    continue
                st = job_stats(res[k])
                if not st:
                    continue
                for f, v in st.items():
                    per_file.setdefault(f, []).append(v)
                job_g.append(gmean(v[0] for v in st.values()))
            if not per_file:
                continue
            med_pr = {f: statistics.median(x[0] for x in v) for f, v in per_file.items()}
            med_aa = {f: statistics.median(x[1] for x in v) for f, v in per_file.items()
                      if all(x[1] is not None for x in v)}
            med_pd = {f: statistics.median(x[2] for x in v) for f, v in per_file.items()}
            perfile[k] = (per_file, med_pr, med_aa)
            worst = min(med_pr, key=med_pr.get)
            best = max(med_pr, key=med_pr.get)
            label = LABEL[k[1]] % k[2] + ('' if k[0] == 'main' else ' (%s)' % k[0])
            aa = aaw = '-'
            if med_aa:
                w = max(med_aa, key=lambda f: abs(med_aa[f]))
                aa, aaw = pct(gmean(med_aa.values())), '%s %s' % (w, pct(med_aa[w]))
            out.append('| %s | **%s** | %s | %s %s | %s %s | %s | %s | %s |' % (
                label, pct(gmean(med_pr.values())), ' '.join(pct(g) for g in job_g),
                worst, pct(med_pr[worst]), best, pct(med_pr[best]), aa, aaw,
                pct(gmean(med_pd.values()))))
        out.append('')
        for k in (('main', 'c', 6), ('main', 'd', 6)):
            if k not in perfile:
                continue
            per_file, med_pr, med_aa = perfile[k]
            ratio = {}
            for _, _, res in members:
                ratio.update(res.get(k, {}).get('ratio', {}))
            out.append('<details><summary>Per file, %s (median over jobs; per job after it)</summary>\n'
                       % (LABEL[k[1]] % k[2]))
            out.append('| file | ratio | PR vs master | per job | A/A |')
            out.append('|---|---|---|---|---|')
            for f in sorted(per_file):
                out.append('| %s | %s | %s | %s | %s |' % (
                    f, ratio.get(f, ''), pct(med_pr[f]), ' '.join(pct(x[0]) for x in per_file[f]),
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
