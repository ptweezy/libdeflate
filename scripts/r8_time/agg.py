#!/usr/bin/env python3
"""r8_time agg.py - pool the shards of one or more r8-time runs (test-only).

usage: agg.py DIR... [--metric auto|wall|cpu|cyc] [--est min|pair] [--md OUT.md]
              [--json OUT.json] [--no-shards]

DIR is searched recursively for shard results: a directory with a
meta.json (shard.sh) and one lt.py JSON per job.  That is what the
workflow's r8-shard-* artifacts contain, so after

    gh run download RUN_ID --repo OWNER/REPO -D runs/RUN_ID

`agg.py runs/RUN_ID` (or several run directories at once) reproduces and
extends the run's own summary.

Shards are grouped by (base commit, architecture, config, file set, level,
chunk).  The unit of analysis is one layout seed of one shard: per seed,
la.py's paired log ratio of the two trees, averaged over the files.  For
every group the script prints the mean over seeds with its 95% CI
half-width (Student t across seeds) and the seed count, pooled over all
shards and separately for every CPU model the shards happened to get:

  f1/m0, f2/f1, f2/m0   the series (m0 = base, fN = base + N commits)
  copy A/A              a byte-identical copy of m0's binary vs m0
  posK                  mkctl.py's positive control, next to the effect
                        predicted from `ldbench -K` on the same runner
  plcN                  mkctl.py's placement-only controls

Marks: '+' CI entirely above 0, '*' CI entirely below 0, '!' some file's
CI entirely below -0.3% (la.py's rule).  Positive effects mean the second
tree is faster.  The end of the output lists every series cell with a CI
below 0, the validation verdicts, and the per-seed spread with the seeds
needed for +-0.1% and +-0.2%.
"""
import contextlib, io, json, math, os, re, sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import la  # noqa: E402

MIDX = {'wall': 2, 'cpu': 3, 'cyc': 4}
CFGNAME = {'g': 'gcc', 'c': 'clang'}


def cfgname(cfg):
    o = {'2': '-O2', '3': '-O3', 's': '-Os', '1': '-O1'}.get(cfg[1:2], cfg[1:])
    return '%s %s%s' % (CFGNAME.get(cfg[0], cfg[0]), o, ' -fPIC' if cfg.endswith('p') else '')


def find_shards(dirs):
    out = []
    for d in dirs:
        for root, _, fs in sorted(os.walk(d)):
            if 'meta.json' not in fs:
                continue
            meta = json.load(open(os.path.join(root, 'meta.json')))
            jobs = {}
            for f in sorted(fs):
                if not f.endswith('.json') or f in ('meta.json', 'sample-map.json'):
                    continue
                try:
                    j = json.load(open(os.path.join(root, f)))
                except ValueError:
                    continue
                if 'data' in j and 'args' in j:
                    jobs[(int(j['args']['l']), int(j['args']['s']))] = j
            out.append({'dir': root, 'meta': meta, 'jobs': jobs})
    return out


def fmt(c, mark=True):
    if c is None:
        return '-'
    s = '%+.3f ±%.3f' % (la.pc(c['m']), c['hw'] * 100)
    if mark:
        if c['m'] - c['hw'] > 0:
            s += ' +'
        elif c['m'] + c['hw'] < 0:
            s += ' *'
        if c['freg']:
            s += ' !'
    return s


def cell(x, files, b, c, uids, est):
    if b not in x or c not in x:
        return None
    gs, rf, _ = la.effects(x, files, b, c, [(u, u) for u in uids], est)
    if len(gs) < 2:
        return None
    m, sd, hw = la.mci(gs)
    fst = {f: la.mci(rf[f]) for f in files}
    worst = min(files, key=lambda f: fst[f][0])
    return {'m': m, 'sd': sd, 'hw': hw, 'S': len(gs),
            'freg': [f for f in files if la.pc(fst[f][0] + fst[f][2]) < -0.3],
            'neg': sum(1 for f in files if fst[f][0] < 0),
            'worst': [worst, fst[worst][0], fst[worst][2]]}


def predict(sh, job, x, files, uids, ctl, metric):
    """Predicted ln ratio of posK vs m0 in one shard: K bytes per multiply,
    the multiply timed by `ldbench -K` in the same job (its minimum)."""
    k = int(ctl[3:])
    cal = [c[MIDX[metric]] for c in job.get('calib', []) if c[MIDX[metric]] > 0]
    if not cal or not uids:
        return None, None
    add = min(cal) / k
    pr = []
    for f in files:
        mb = sum(min(x['m0'][u][f]) for u in uids) / len(uids)
        pr.append(math.log(mb / (mb + add)))
    return sum(pr) / len(pr), min(cal)


def main():
    args = sys.argv[1:]
    metric = la.opt(args, '--metric', 'auto')
    est = la.opt(args, '--est', 'min')
    md = la.opt(args, '--md', None)
    jsout = la.opt(args, '--json', None)
    noshards = '--no-shards' in args
    dirs = [a for a in args if not a.startswith('--')]
    if not dirs:
        sys.exit(__doc__)
    shards = find_shards(dirs)
    if not shards:
        sys.exit('no shard results (meta.json) under ' + ' '.join(dirs))
    L = []
    P = L.append
    runs = sorted(set('%s/%s' % (s['meta'].get('repo', '?'), s['meta'].get('run_id', '?')) for s in shards))
    P('# r8-time: %d shards, metric %s, estimator %s' % (len(shards), metric, est))
    P('')
    P('Runs: ' + ', '.join(runs))
    P('')

    # ---- groups ----
    groups = {}
    for si, s in enumerate(shards):
        m = s['meta']
        for (lv, ch), j in s['jobs'].items():
            key = (m.get('base_sha', '?')[:7], m['arch'], m['cfg'], m.get('fileset', '?'),
                   m.get('ltopts', ''), ch != 0, ch, lv, tuple(j['files']))
            groups.setdefault(key, []).append((si, j))
    cells = []      # every cell, for the lists at the end and --json
    for key in sorted(groups):
        base, arch, cfg, fileset, lto, _, ch, lv, files = key
        files = list(files)
        mem = groups[key]
        # auto: user-mode cycles if every shard of the group counted them
        # (the Arm runners have a PMU), else wall-clock nanoseconds
        gm = metric
        if metric == 'auto':
            gm = 'cyc' if all('data_cyc' in j for _, j in mem) else 'wall'
        x = {}
        uid_shard = {}
        for si, j in mem:
            _, xs, seeds, _ = la.load(j, gm)
            for t in xs:
                for sd in xs[t]:
                    u = '%s:%s' % (shards[si]['meta']['name'], sd)
                    if t == 'm0' and u in uid_shard:
                        print('warning: seed %s twice in %s' % (u, key), file=sys.stderr)
                    x.setdefault(t, {})[u] = xs[t][sd]
                    uid_shard[u] = si
        uids = sorted(x.get('m0', {}))
        trees = [t for t in ['m0'] + sorted(t for t in x if re.match(r'^f\d+$', t)) if t in x]
        series = [(trees[i - 1], trees[i]) for i in range(1, len(trees))]
        if len(trees) > 2:
            series.append((trees[0], trees[-1]))
        ctls = sorted(t for t in x if t not in trees and t != 'AA')
        cmps = series + ([('m0', 'AA')] if 'AA' in x else []) + [('m0', c) for c in ctls]
        nr = sorted(set('%sx%s' % (j.get('rounds_done', j['args']['n']), j['args']['r']) for _, j in mem))
        mism = sum(j['hash_mismatches'] for _, j in mem)
        title = '%s %s, %s files, L%d %s%s' % (cfgname(cfg), arch, fileset, lv,
                                               'whole' if ch == 0 else 'chunk %d' % ch,
                                               (' [lt.py %s]' % lto) if lto else '')
        P('## %s (base %s)' % (title, base))
        P('')
        P('%d shards, %d seeds, rounds x reps %s, %d files, clock: %s, hash mismatches %d%s' % (
            len(mem), len(uids), '/'.join(nr), len(files),
            {'wall': 'wall ns', 'cpu': 'thread CPU ns', 'cyc': 'user-mode cycles'}[gm], mism,
            ' **OUTPUT DIFFERS**' if mism else ''))
        P('')
        hdr = ['CPU model', 'shards', 'S'] + ['%s/%s' % (c, b) if c != 'AA' else 'copy A/A' for b, c in cmps]
        P('| ' + ' | '.join(hdr) + ' |')
        P('|' + '---|' * len(hdr))
        models = {}
        for u in uids:
            models.setdefault(shards[uid_shard[u]]['meta']['cpu_model'], []).append(u)
        rows = [('pooled', uids)] + sorted(models.items(), key=lambda kv: -len(kv[1]))
        detail = []
        if len(models) == 1:
            rows = [('pooled = ' + rows[1][0], uids)]
        for rname, ru in rows:
            sis = sorted(set(uid_shard[u] for u in ru))
            row = [rname, str(len(sis)), str(len(ru))]
            for b, c in cmps:
                ce = cell(x, files, b, c, ru, est)
                txt = fmt(ce, mark=not c.startswith('pos'))
                rec = {'group': title, 'base': base, 'arch': arch, 'cfg': cfg, 'fileset': fileset,
                       'level': lv, 'chunk': ch, 'row': rname, 'cmp': '%s/%s' % (c, b), 'metric': gm,
                       'shards': len(sis), 'rounds': '/'.join(nr),
                       'kind': 'series' if (b, c) in series else 'aa' if c == 'AA' else
                               'pos' if c.startswith('pos') else 'plc', 'cell': ce}
                if ce and c.startswith('pos'):
                    # prediction: seed-weighted mean of the shards' predictions
                    num = den = 0.0
                    cals = []
                    for si in sis:
                        su = [u for u in ru if uid_shard[u] == si and u in x[c]]
                        job = [j for s2, j in mem if s2 == si][0]
                        pr, cal = predict(shards[si], job, x, files, su, c, gm)
                        if pr is not None:
                            num += pr * len(su)
                            den += len(su)
                            cals.append(cal)
                    if den:
                        pred = num / den
                        rec['pred'] = pred
                        rec['calib'] = [min(cals), max(cals)]
                        txt += ' (pred %+.3f, x%.2f%s)' % (la.pc(pred), ce['m'] / pred,
                                                           '' if abs(ce['m'] - pred) <= ce['hw'] else ' OUTSIDE CI')
                row.append(txt)
                cells.append(rec)
            P('| ' + ' | '.join(row) + ' |')
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                for b, c in cmps:
                    if b in x and c in x:
                        la.compare(x, files, b, c, [(u, u) for u in ru], True, '%s/%s' % (c, b), False, est)
            detail.append('-- %s: %d shards, %d seeds\n%s' % (rname, len(sis), len(ru), buf.getvalue().rstrip()))
        P('')
        P('<details><summary>la.py lines%s</summary>' % ('' if noshards else ' and per-shard table'))
        P('')
        P('```')
        for dt in detail:
            P(dt)
        P('```')
        if not noshards:
            P('')
            P('| shard | CPU model | S | rounds | m0 %s/byte | copy A/A | %s/m0 | load max |' % (
                'cyc' if gm == 'cyc' else 'ns', trees[-1]))
            P('|---|---|---|---|---|---|---|---|')
            for si, j in mem:
                su = [u for u in uids if uid_shard[u] == si]
                if not su:
                    continue
                sp = math.exp(sum(math.log(sum(min(x['m0'][u][f]) for u in su) / len(su)) for f in files) / len(files))
                P('| %s | %s | %d | %s | %.3f | %s | %s | %.2f |' % (
                    shards[si]['meta']['name'], shards[si]['meta']['cpu_model'], len(su),
                    j.get('rounds_done', '?'), sp, fmt(cell(x, files, 'm0', 'AA', su, est)),
                    fmt(cell(x, files, 'm0', trees[-1], su, est)), max(j['loads']) if j['loads'] else -1))
        P('')
        P('</details>')
        P('')

    # ---- lists ----
    P('## Series cells with a CI entirely below 0')
    P('')
    bad = [c for c in cells if c['kind'] == 'series' and c['cell'] and c['cell']['m'] + c['cell']['hw'] < 0]
    for c in bad:
        P('- %s (base %s), %s, %s: %s (S=%d)' % (c['group'], c['base'], c['row'], c['cmp'], fmt(c['cell']),
                                                   c['cell']['S']))
    if not bad:
        P('None.')
    P('')
    P('## Series cells where a file\'s CI is entirely below -0.3%')
    P('')
    fr = [c for c in cells if c['kind'] == 'series' and c['cell'] and c['cell']['freg']]
    for c in fr:
        P('- %s (base %s), %s, %s: %s; files %s; worst %s %+.3f ±%.3f' % (
            c['group'], c['base'], c['row'], c['cmp'], fmt(c['cell']), ','.join(c['cell']['freg']),
            c['cell']['worst'][0], la.pc(c['cell']['worst'][1]), c['cell']['worst'][2] * 100))
    if not fr:
        P('None.')
    P('')
    P('## Validation')
    P('')
    for kind, name in (('aa', 'Copy A/A'), ('plc', 'Placement-only controls')):
        cs = [c for c in cells if c['kind'] == kind and c['cell']]
        out = [c for c in cs if abs(c['cell']['m']) > c['cell']['hw']]
        P('%s: %d cells (pooled and per CPU model), %d with a CI excluding 0 (5%% expected by chance).' % (
            name, len(cs), len(out)))
        for c in out:
            P('- %s (base %s), %s, %s: %s (S=%d)' % (c['group'], c['base'], c['row'], c['cmp'],
                                                       fmt(c['cell'], False), c['cell']['S']))
        P('')
    cs = [c for c in cells if c['kind'] == 'pos' and c['cell'] and 'pred' in c]
    P('Positive controls: %d cells.' % len(cs))
    if cs:
        P('')
        P('| group | CPU model | S | control | predicted | measured | measured/predicted | pred within CI | multiply ns |')
        P('|---|---|---|---|---|---|---|---|---|')
        for c in cs:
            ce = c['cell']
            P('| %s (base %s) | %s | %d | %s | %+.3f | %+.3f ±%.3f | %.2f | %s | %.3f-%.3f |' % (
                c['group'], c['base'], c['row'], ce['S'], c['cmp'], la.pc(c['pred']), la.pc(ce['m']),
                ce['hw'] * 100, ce['m'] / c['pred'], 'yes' if abs(ce['m'] - c['pred']) <= ce['hw'] else 'NO',
                c['calib'][0], c['calib'][1]))
    P('')
    P('## Per-seed spread and seeds needed')
    P('')
    P('sd = standard deviation across seeds of the per-seed geomean, in %; seeds for a 95% CI half-width of')
    P('0.1% / 0.2% at that sd.')
    P('')
    P('| group | CPU model | S | comparison | sd | seeds ±0.1% | seeds ±0.2% |')
    P('|---|---|---|---|---|---|---|')
    for c in cells:
        ce = c['cell']
        if not ce or c['kind'] not in ('series', 'aa'):
            continue
        last = c['kind'] == 'aa' or re.match(r'^f\d+/m0$', c['cmp'])
        if not last:
            continue
        P('| %s (base %s) | %s | %d | %s | %.3f | %d | %d |' % (
            c['group'], c['base'], c['row'], ce['S'], c['cmp'], ce['sd'] * 100,
            la.need(ce['sd'], 0.1), la.need(ce['sd'], 0.2)))
    P('')

    # ---- shards ----
    P('## Shards')
    P('')
    cm = {}
    for s in shards:
        m = s['meta']
        k = (m['arch'], m['cpu_model'])
        cm.setdefault(k, [0, set()])
        cm[k][0] += 1
        cm[k][1].add(m.get('timer', '?'))
    P('| arch | CPU model | shards | timer |')
    P('|---|---|---|---|')
    for k in sorted(cm):
        P('| %s | %s | %d | %s |' % (k[0], k[1], cm[k][0], '; '.join(sorted(cm[k][1]))))
    P('')
    if not noshards:
        P('<details><summary>every shard</summary>')
        P('')
        P('| shard | run | CPU model | kernel | compiler | seeds | jobs | fscheck | check | shield | inputs |')
        P('|---|---|---|---|---|---|---|---|---|---|---|')
        for s in shards:
            m = s['meta']
            comp = m['gcc'] if m['cfg'].startswith('g') else m['clang']
            P('| %s | %s | %s | %s | %s | %s | %d/%d | %s | %s | %s | %s |' % (
                m['name'], m.get('run_id', '?'), m['cpu_model'], m['kernel'], comp, m['seeds'],
                len(s['jobs']), len(m['jobs'].split()), m.get('fscheck', '?'), m.get('check', '?'),
                m.get('shield', '?'), m.get('inputs', '?')))
        P('')
        P('</details>')
        P('')
    text = '\n'.join(L)
    sys.stdout.buffer.write(text.encode('utf-8') + b'\n')
    sys.stdout.flush()
    if md:
        open(md, 'w', encoding='utf-8', newline='\n').write(text + '\n')
    if jsout:
        json.dump({'metric': metric, 'est': est, 'cells': cells,
                   'shards': [s['meta'] for s in shards]}, open(jsout, 'w'), indent=1)


if __name__ == '__main__':
    main()
