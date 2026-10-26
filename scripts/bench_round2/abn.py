#!/usr/bin/env python3
"""
Interleaved multi-variant screen for ldbench (first variant is the baseline).

usage: abn.py [-n ROUNDS] [-r REPS] [-l LEVELS] [-m c|d] [-s CHUNK] [--core N]
              [--files F1,F2,...] BASE V1 [V2 ...]

Each round runs every variant once over all files, rotating the order each
round.  Per file and variant: best-of-REPS cycles/byte per round; the speedup
vs BASE is computed per round (paired) and the median is reported, plus a
geomean over files.  Also checks that compressed sizes match the baseline.
"""
import argparse, math, os, statistics, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('-n', type=int, default=3)
ap.add_argument('-r', type=int, default=3)
ap.add_argument('-l', default='6')
ap.add_argument('-m', default='c')
ap.add_argument('-s', default='0')
ap.add_argument('--core', default='4')
ap.add_argument('--files', default='')
ap.add_argument('variants', nargs='+')
args = ap.parse_args()

home = os.path.expanduser('~')
CORPUS = os.environ.get('LDCORPUS', os.path.join(home, 'corpus'))
LDDIR = os.environ.get('LDDIR', os.path.join(home, 'ld'))
S = os.path.join(CORPUS, 'silesia')
ENWIK8 = os.path.join(CORPUS, 'enwik8')
import shutil
PIN = ['taskset', '-c', args.core] if shutil.which('taskset') else []
if args.files == 'all':
    files = sorted(os.path.join(S, f) for f in os.listdir(S) if not f.startswith('.')) + ([ENWIK8] if os.path.exists(ENWIK8) else [])
elif args.files:
    files = [f if f.startswith('/') else (os.path.join(S, f) if f != 'enwik8' else ENWIK8)
             for f in args.files.split(',')]
else:
    files = [os.path.join(S, f) for f in ('dickens', 'mozilla', 'xml', 'x-ray', 'nci', 'samba')]
col = 3 if args.m == 'c' else 6

def run(variant, level):
    exe = os.path.join(LDDIR, variant, 'ldbench')
    out = subprocess.run(PIN + [exe, '-H', '-m', args.m, '-l', str(level),
                          '-r', str(args.r), '-s', args.s] + files, capture_output=True, text=True,
                         env=dict(os.environ, LDBENCH_QUIET='1'))
    if out.returncode != 0:
        sys.exit(f'{variant} failed: {out.stderr}')
    res = {}
    for line in out.stdout.splitlines():
        p = line.split()
        res[p[0]] = (float(p[col]), p[-1])
    return res

V = args.variants
for level in args.l.split(','):
    data = {v: {} for v in V}
    for rnd in range(args.n):
        k = rnd % len(V)
        order = V[k:] + V[:k]
        if rnd % 2:
            order = order[::-1]
        for v in order:
            for f, val in run(v, level).items():
                data[v].setdefault(f, []).append(val)
        print(f'L{level} round {rnd + 1}/{args.n} done', file=sys.stderr, flush=True)
    names = list(data[V[0]])
    print(f'mode={args.m} level={level} rounds={args.n} reps={args.r} chunk={args.s}  base={V[0]}')
    print(f'{"file":<10} {"base cpb":>9} ' + ' '.join(f'{v[:14]:>15}' for v in V[1:]))
    geo = {v: [] for v in V[1:]}
    for f in names:
        b = [x[0] for x in data[V[0]][f]]
        row = f'{f:<10} {statistics.median(b):9.3f} '
        for v in V[1:]:
            x = [y[0] for y in data[v][f]]
            sp = statistics.median(bb / xx for bb, xx in zip(b, x))
            geo[v].append(sp)
            same = '' if data[v][f][0][1] == data[V[0]][f][0][1] else '*'
            row += f' {(sp - 1) * 100:+13.2f}%{same}'
        print(row)
    print(f'{"GEOMEAN":<10} {"":>9} ' + ' '.join(
        f' {(math.exp(sum(map(math.log, geo[v])) / len(geo[v])) - 1) * 100:+13.2f}%' for v in V[1:]))
    print('(* = compressed output differs from base)', flush=True)
