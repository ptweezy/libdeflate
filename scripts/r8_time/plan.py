#!/usr/bin/env python3
"""r8_time plan.py: turn a plan file into the workflow's shard matrix.

usage: plan.py PLAN.txt            prints "matrix=<JSON>" (for $GITHUB_OUTPUT)
       plan.py -v PLAN.txt         prints one line per shard instead

Plan file, one directive per line ('#' starts a comment):

  seedbase N     first seed number (default 1).  Seeds are handed out in
                 file order and never repeat within a plan; give a later
                 run a different seedbase to add independent layouts.
  series N       commits in the series under test (default 2): the trees are
                 m0 = HEAD~(N+1), f1 = HEAD~N, ..., fN = HEAD~1; HEAD is the
                 test-only commit.
  timeout MIN    job timeout for the groups that follow (default 150).  A
                 shard stops starting rounds when it would run out of time.
  ltopts OPTS    extra lt.py options for the groups that follow, e.g.
                 "ltopts --order rot"; "ltopts" alone clears them.  agg.py
                 never pools shards with different options.
  group NAME RUNNER CFG SHARDS SEEDS FILESET JOB...
                 SHARDS jobs on RUNNER (ubuntu-24.04, ubuntu-24.04-arm, ...),
                 each building lb.py config CFG (g2, c2, g3, ...) and
                 linking its own block of SEEDS layout seeds, then timing
                 every JOB on FILESET (a directory made by inputs.sh: real,
                 adv, syn).
                 JOB = LEVEL:CHUNK:ROUNDS:REPS[:CTL+CTL...]; CHUNK 0 = whole
                 files; CTL = control trees timed along with m0, f1..fN and
                 the copy A/A (mkctl.py: pos64, pos16, plc1, plc2, ...).

Shards are started in file order, so put what you want first at the top.
"""
import json, sys

verbose = '-v' in sys.argv
args = [a for a in sys.argv[1:] if a != '-v']
seed = 1
series = 2
timeout = 150
ltopts = ''
out = []
names = set()
for ln, line in enumerate(open(args[0]), 1):
    p = line.split('#')[0].split()
    if not p:
        continue
    if p[0] == 'seedbase':
        seed = int(p[1])
    elif p[0] == 'series':
        series = int(p[1])
    elif p[0] == 'timeout':
        timeout = int(p[1])
    elif p[0] == 'ltopts':
        ltopts = ' '.join(p[1:])
    elif p[0] == 'group':
        if len(p) < 8:
            sys.exit('%s:%d: group needs NAME RUNNER CFG SHARDS SEEDS FILESET JOB...' % (args[0], ln))
        name, runner, cfg, shards, n, fileset = p[1], p[2], p[3], int(p[4]), int(p[5]), p[6]
        for j in p[7:]:
            f = j.split(':')
            if len(f) not in (4, 5) or not all(x.isdigit() for x in f[:4]):
                sys.exit('%s:%d: bad job %r' % (args[0], ln, j))
        arch = 'a64' if 'arm' in runner else 'x64'
        for i in range(shards):
            nm = '%s-%s-%s-%02d' % (name, cfg, arch, i)
            if nm in names:
                sys.exit('%s:%d: duplicate shard name %s' % (args[0], ln, nm))
            names.add(nm)
            out.append({'name': nm, 'group': name, 'runner': runner, 'cfg': cfg,
                        'seeds': '%d-%d' % (seed, seed + n - 1), 'fileset': fileset,
                        'jobs': ' '.join(p[7:]), 'series': series, 'timeout': timeout,
                        'ltopts': ltopts})
            seed += n
    else:
        sys.exit('%s:%d: unknown directive %r' % (args[0], ln, p[0]))
if not out:
    sys.exit('empty plan')
if len(out) > 256:
    sys.exit('%d shards: a matrix can have at most 256 jobs' % len(out))
if verbose:
    for o in out:
        print('%-24s %-18s %-4s seeds %-11s %-5s %s%s' % (o['name'], o['runner'], o['cfg'], o['seeds'],
                                                        o['fileset'], o['jobs'],
                                                        ('  [' + o['ltopts'] + ']') if o['ltopts'] else ''))
    print('%d shards, seeds up to %d' % (len(out), seed - 1))
else:
    print('matrix=' + json.dumps(out, separators=(',', ':')))
