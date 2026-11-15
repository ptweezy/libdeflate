#!/usr/bin/env python3
"""r8_time merge.py: merge lt.py JSON files of the same job (same level,
chunk, trees, files) by concatenating the per-(tree, seed, file) round
lists, so la.py takes the min over all rounds of all runs.  Seeds present
in only some runs keep what they have.  (agg.py does its own pooling of
shards; this is for combining reruns of one shard by hand.)
usage: merge.py OUT.json IN1.json IN2.json ..."""
import json, sys
out, ins = sys.argv[1], sys.argv[2:]
ds = [json.load(open(f)) for f in ins]
base = ds[0]
for d in ds[1:]:
    for k in ('l', 's'):
        if d['args'][k] != base['args'][k]:
            sys.exit('mismatch %s: %s vs %s' % (k, d['args'][k], base['args'][k]))
trees = list(base['trees'])
seeds = sorted(set(s for d in ds for s in d['seeds']))
files = list(base['files'])
m = dict(base)
for key in ('data', 'data_cpu', 'data_cyc'):
    if not all(key in d for d in ds):
        m.pop(key, None)
        continue
    data = {t: {str(s): {f: [] for f in files} for s in seeds} for t in trees}
    for d in ds:
        for t in trees:
            for s in d[key].get(t, {}):
                for f in files:
                    data[t][s][f] += d[key][t][s].get(f, [])
    m[key] = data
m['seeds'] = seeds
m['hash_mismatches'] = sum(d['hash_mismatches'] for d in ds)
m['rounds_done'] = sum(d.get('rounds_done', d['args']['n']) for d in ds)
m['args'] = dict(base['args'], n=m['rounds_done'], o=out)
m['loads'] = sum((d['loads'] for d in ds), [])
m['calib'] = sum((d.get('calib', []) for d in ds), [])
m['merged_from'] = ins
json.dump(m, open(out, 'w'))
print('merged %d files -> %s (%d rounds)' % (len(ins), out, m['rounds_done']))
