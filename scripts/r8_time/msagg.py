#!/usr/bin/env python3
"""msagg.py DIR PAIRS  (test-only): tables from msab.py TSVs.

DIR holds r13-msvc-ARCH-SET-SHARD/res.tsv per job.  PAIRS is a comma-separated
list of A:B; a cell is (t_A / t_B - 1) * 100, so + means B is faster.  The
main number uses each exe's minimum over all shards of one arch; the range in
parentheses is the same cell computed within each shard.
"""
import glob, os, sys, statistics

root, pairs = sys.argv[1], [p.split(':') for p in sys.argv[2].split(',')]
pooled, shard, order, cpus, diff = {}, {}, {}, {}, 0
for path in sorted(glob.glob(os.path.join(root, '*', 'res.tsv'))):
    job = os.path.basename(os.path.dirname(path))
    arch = job.split('-')[-3]
    cpu = os.path.join(os.path.dirname(path), 'cpu.txt')
    if os.path.exists(cpu):
        cpus.setdefault(arch, []).append(
            job + ': ' + ' '.join(open(cpu).read().split()))
    for line in open(path):
        lvl, chunk, f, name, t, _, same = line.rstrip('\n').split('\t')
        diff += same != 'same'
        cfg = lvl + ':' + chunk
        k = (arch, cfg, f, name)
        pooled[k] = min(pooled.get(k, 1e30), float(t))
        shard[(job,) + k] = float(t)
        order.setdefault(arch, {}).setdefault(f, None)
print('output hash mismatches (rows): %d\n' % diff)
for arch in sorted(order):
    print('## MSVC %s\n' % arch)
    for c in cpus.get(arch, []):
        print('- ' + c)
    print()
    cfgs = []
    for k in pooled:
        if k[0] == arch and k[1] not in cfgs:
            cfgs.append(k[1])
    jobs = sorted({k[0] for k in shard if k[1] == arch})
    for a, b in pairs:
        print('### %s -> %s (+ = %s faster)\n' % (a, b, b))
        print('| file | ' + ' | '.join('L' + c for c in cfgs) + ' |')
        print('|---|' + '---:|' * len(cfgs))
        means = {c: [] for c in cfgs}
        for f in order[arch]:
            row = []
            for c in cfgs:
                ta, tb = pooled.get((arch, c, f, a)), pooled.get((arch, c, f, b))
                if ta is None or tb is None:
                    row.append('')
                    continue
                d = (ta / tb - 1) * 100
                means[c].append(d)
                per = [(shard[(j, arch, c, f, a)] / shard[(j, arch, c, f, b)] - 1) * 100
                       for j in jobs if (j, arch, c, f, a) in shard
                       and (j, arch, c, f, b) in shard]
                row.append('%+.2f' % d + (' (%+.1f..%+.1f)' % (min(per), max(per))
                                          if len(per) > 1 else ''))
            print('| %s | ' % f + ' | '.join(row) + ' |')
        print('| **mean** | ' + ' | '.join(
            '%+.2f' % statistics.mean(means[c]) if means[c] else '' for c in cfgs) + ' |\n')
