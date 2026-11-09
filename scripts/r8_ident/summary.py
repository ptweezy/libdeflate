#!/usr/bin/env python3
"""Summarize the results of all the jobs of an r8-ident run.

usage: summary.py OUTDIR [MATRIX_JSON]

OUTDIR has one directory per job, as left by run.sh (verdict.tsv, meta.txt,
base/*.txt).  MATRIX_JSON is the job matrix; jobs in it that left no results
are reported as missing.

Prints Markdown:
 1. per platform: every candidate's verdict against base on that platform;
 2. per job: details and times;
 3. whether base's outputs are the same on all platforms (informational).
    Each tool's output is compared in full, and also after removing the
    hashes that cover more than the compressed bytes themselves (xcheck and
    flvtest all=, mdrv short=, vid h), because too-small output buffers and
    the bytes after the end of the output may be written differently on
    different platforms without the compressed streams differing.

Exit status: 1 if any candidate is not identical to base somewhere, or if a
job is missing or failed.
"""
import collections
import hashlib
import json
import os
import re
import sys

outdir = sys.argv[1]
expected = []
if len(sys.argv) > 2 and sys.argv[2]:
    expected = [(j['name'], j['platform'])
                for j in json.loads(sys.argv[2])['include']]

STRIP = {
    'xcheck': re.compile(rb' all=[0-9a-f]+'),
    'flv': re.compile(rb' all=[0-9a-f]+'),
    'mdrv': re.compile(rb' short=[0-9a-f]+'),
    'vid': re.compile(rb' h[0-9a-f]{16}$', re.M),
}


def portable(tool, data):
    if tool == 'vid':  # the per-level digest lines are made from h
        data = b''.join(ln for ln in data.splitlines(True)
                        if not ln.startswith(b'LEVEL'))
    if tool in STRIP:
        data = STRIP[tool].sub(b'', data)
    return data


jobs = {}       # job name -> dict
for name in sorted(os.listdir(outdir)):
    d = os.path.join(outdir, name)
    if not os.path.isfile(os.path.join(d, 'verdict.tsv')):
        continue
    j = dict(name=name, platform=name.split('.')[0], rows=[], meta={},
             metalines=[])
    with open(os.path.join(d, 'verdict.tsv')) as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) >= 5:
                j['rows'].append(dict(task=p[1], lines=int(p[2] or 0),
                                      base=p[3], verdicts=p[4:-1],
                                      secs=p[-1]))
            else:
                j['failed'] = p[-1]
    try:
        with open(os.path.join(d, 'meta.txt')) as f:
            for line in f:
                line = line.rstrip('\n')
                j['metalines'].append(line)
                if '=' in line and not line.startswith(('#', ' ')):
                    k, v = line.split('=', 1)
                    j['meta'].setdefault(k, v)
    except OSError:
        pass
    jobs[name] = j

platforms = []
for name, plat in expected:
    if plat not in platforms:
        platforms.append(plat)
for j in jobs.values():
    if j['platform'] not in platforms:
        platforms.append(j['platform'])
missing = [name for name, _ in expected if name not in jobs]
ncand = max([len(r['verdicts']) for j in jobs.values() for r in j['rows']] or
            [0])
bad = bool(missing)
out = []
w = out.append

# ---- 1. verdicts per platform ----
w('## Candidates against base, per platform')
w('')
w('| platform | jobs | tasks | lines of base output | base runs | ' +
  ' | '.join('c%d vs base' % (i + 1) for i in range(ncand)) + ' |')
w('|---|---|---|---|---|' + '---|' * ncand)
for plat in platforms:
    pj = [j for j in jobs.values() if j['platform'] == plat]
    exp = [n for n, p in expected if p == plat]
    miss = [n for n in exp if n not in jobs]
    rows = [r for j in pj for r in j['rows']]
    failed = [j['name'] + ': ' + j['failed'] for j in pj if 'failed' in j]
    cells = []
    for i in range(ncand):
        vs = collections.Counter(r['verdicts'][i] for r in rows
                                 if i < len(r['verdicts']))
        if not rows or miss or failed:
            cells.append('INCOMPLETE' if not vs or set(vs) == {'IDENTICAL'}
                         else 'NOT IDENTICAL')
        elif set(vs) == {'IDENTICAL'}:
            cells.append('IDENTICAL')
        else:
            cells.append('**NOT IDENTICAL** (' + ', '.join(
                '%d %s' % (n, v) for v, n in sorted(vs.items())
                if v != 'IDENTICAL') + ')')
    nbase_bad = sum(r['base'] != 'ok' for r in rows)
    if nbase_bad or miss or failed or any(c != 'IDENTICAL' for c in cells):
        bad = True
    note = ''
    if miss:
        note += ' missing: ' + ', '.join(miss)
    if failed:
        note += ' ' + '; '.join(failed)
    w('| %s | %d%s | %d | %d | %s | %s |' % (
        plat, len(pj), note, len(rows), sum(r['lines'] for r in rows),
        'ok' if rows and not nbase_bad else '%d not ok' % nbase_bad,
        ' | '.join(cells)))
w('')
w('Overall: ' + ('**NOT all identical, or incomplete**' if bad else
                 'every candidate is identical to base on every platform'))
w('')

# ---- 2. per job ----
w('## Jobs')
w('')
w('| job | exec | compiler | endian / bits / unaligned fast | profile | '
  'lines per tool | seconds (build, run, total) |')
w('|---|---|---|---|---|---|---|')
for name in [n for n, _ in expected if n in jobs] + \
        [n for n in jobs if n not in dict(expected)]:
    j = jobs[name]
    ml = j['metalines']

    def find(prefix, default=''):
        for line in ml:
            if line.startswith(prefix):
                return line[len(prefix):]
        return default
    probe = ' '.join(line[7:] for line in ml if line.startswith('probe: '))
    m = re.search(r'runtime_little_endian=(\d)', probe)
    endian = {'1': 'little', '0': 'BIG'}.get(m.group(1) if m else '', '?')
    m = re.search(r'pointer_bits=(\d+)', probe)
    bits = m.group(1) if m else '?'
    m = re.search(r'UNALIGNED_ACCESS_IS_FAST=(\d)', probe)
    ua = m.group(1) if m else '?'
    per_tool = collections.OrderedDict()
    for r in j['rows']:
        t = r['task'].split('.')[0]
        per_tool[t] = per_tool.get(t, 0) + r['lines']
    w('| %s | %s | %s | %s / %s / %s | %s | %s | %s, %s, %s |' % (
        name, find('exec='), find('compiler: ') or find('probe: compiler='),
        endian, bits, ua, find('profile='),
        ', '.join('%s %d' % kv for kv in per_tool.items()),
        find('built in ').rstrip('s'), find('ran in ').rstrip('s'),
        find('total_seconds=')))
w('')

# ---- 3. base across platforms ----
w('## Base outputs across platforms (informational)')
w('')
full = collections.defaultdict(dict)    # task -> platform -> sha
port = collections.defaultdict(dict)
for j in jobs.values():
    bdir = os.path.join(outdir, j['name'], 'base')
    if not os.path.isdir(bdir):
        continue
    for fn in sorted(os.listdir(bdir)):
        if not fn.endswith('.txt'):
            continue
        task = fn[:-4]
        with open(os.path.join(bdir, fn), 'rb') as f:
            data = f.read()
        tool = task.split('.')[0]
        full[task][j['platform']] = hashlib.sha256(data).hexdigest()
        port[task][j['platform']] = hashlib.sha256(
            portable(tool, data)).hexdigest()

tools = []
for task in full:
    t = task.split('.')[0]
    if t not in tools:
        tools.append(t)


def partition(table, tasks, plats):
    """Group the platforms whose outputs agree on all the given tasks."""
    groups = collections.OrderedDict()
    for p in plats:
        sig = tuple(table[t].get(p) for t in tasks)
        groups.setdefault(sig, []).append(p)
    return list(groups.values())


def describe(groups, nplats):
    if len(groups) == 1:
        return 'the same on all %d' % nplats
    return '%d different outputs: ' % len(groups) + ' / '.join(
        '{' + ', '.join(g) + '}' for g in groups)


w('| tool | tasks | platforms | whole output | compressed bytes only |')
w('|---|---|---|---|---|')
for tool in sorted(tools):
    ttasks = sorted(t for t in full if t.split('.')[0] == tool)
    # Tasks are compared among the platforms that ran them.  Normally that
    # gives two rows per tool: the tasks that every platform ran, and the
    # extra tasks of the platforms that run the binaries natively.
    by_plats = collections.OrderedDict()
    for t in ttasks:
        plats = tuple(p for p in platforms if p in full[t])
        by_plats.setdefault(plats, []).append(t)
    for plats, tasks in sorted(by_plats.items(), key=lambda kv: -len(kv[0])):
        if len(plats) < 2:
            continue
        g1 = partition(full, tasks, plats)
        g2 = partition(port, tasks, plats)
        w('| %s | %d | %d%s | %s | %s |' % (
            tool, len(tasks), len(plats),
            '' if len(plats) == len(platforms) else
            ': ' + ', '.join(plats),
            describe(g1, len(plats)),
            describe(g2, len(plats)) if tool in STRIP else 'n/a'))
w('')
w('"compressed bytes only" leaves out the hashes that also cover the calls '
  'with too-small output buffers, whole output buffers and decompression '
  '(xcheck and flvtest `all=`, mdrv `short=`, vid `h`).')
w('')
print('\n'.join(out))
sys.exit(1 if bad else 0)
