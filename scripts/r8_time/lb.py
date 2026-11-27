#!/usr/bin/env python3
"""r8_time lb.py - randomized-layout builds of libdeflate's compressor
(test-only; the CI port of handoff/tools/layout-harness/lb.py: native gcc or
clang on x86-64 or aarch64, GNU ld).

  lb.py obj     CFG TREE...            compile objects once per (config, tree)
  lb.py link    CFG MODE SEEDS TREE... link one binary per (tree, seed)
  lb.py fscheck CFG TREE...            does -ffunction-sections change the asm?
  lb.py check   CFG MODE SEEDS TREE... code and data identity across layouts

TREE is a source tree: a name N for $R8LAY_SRC/N-src, or an absolute path
(its basename, minus a trailing -src, is then the tree's name).  CFG is a
key of CFGS below.  SEEDS is e.g. 1-32 or 1,5,9 or "plain".

Environment:
  R8LAY_ROOT     output root: objects in ROOT-o, binaries in ROOT-b (required)
  R8LAY_SRC      directory holding the NAME-src trees (required for names)
  R8LAY_LDBENCH  path of ldbench.c (default: next to this script)
  R8LAY_PREFIX   command prefix for compilers and linkers, e.g.
                 "nice -n 19 taskset -c 12-15" (default: none)
  R8LAY_CFLAGS   extra CFLAGS for every object
  R8LAY_P        anchor modulus (default 4096)

Objects: lib/deflate_compress.c is compiled with -ffunction-sections (the
compiler's output is otherwise unchanged; `lb.py fscheck` checks that),
every other file exactly as handoff/tools/bench/build.sh does.  Nothing is
recompiled per seed: a seed only changes the link, so each function's code
is the same in every layout of a tree (`lb.py check` checks that).

Layout of one seed: every executable section of deflate_compress.o goes
into one output section .text.r8lay (4096-aligned, inserted before .text
with an INSERT script, so the default linker script stays in charge of
everything else), in the object's order (MODE pad/anchor) or in a random
order (MODE padshuf/anchorshuf).  Random numbers come from
sha256("SEED:KIND:NAME"), so a function gets the same pad, target and
order key in every tree; a function that exists in only one tree is
placed by the same rule.
  pad:    pad before each function = uniform multiple of its alignment in
          [0, P), plus a random entry offset in [0, P) before the first.
  anchor: each function starts at address = uniform target (multiple of its
          alignment) mod P; the pad before it is whatever reaches that
          target, so functions present in both trees have the same low
          log2(P) address bits in both, whatever the sizes of the functions
          before them; the entry offset is a random multiple of P in
          [0, 16P), which moves the higher address bits.
Plain: seed "plain" = build.sh's link (archive, no function sections, no
script).

aarch64: gcc links with -mno-fix-cortex-a53-843419 (the erratum workaround
that Debian/Ubuntu's gcc enables by default makes GNU ld rewrite ADRP
instructions that land at page offsets 0xff8/0xffc, i.e. the code would
depend on the layout), and both compilers with -z separate-code (x86-64's
default: .rodata then starts on a page boundary, so data keeps its address
mod 4096 whatever the size of the text).

Output ROOT-b/CFG.MODE/TREE/sNNN/{ldbench,lay.ld,map.json}; plain builds
in ROOT-b/CFG.plain/TREE/s000/.  map.json has every deflate_compress.c
function's address and size, the pads, and the data symbols.  After each
link the addresses are checked against the planned layout (nm).
"""
import hashlib, json, os, platform, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get('R8LAY_ROOT')
SRCROOT = os.environ.get('R8LAY_SRC', '')
LDBENCH = os.environ.get('R8LAY_LDBENCH', os.path.join(HERE, 'ldbench.c'))
P = int(os.environ.get('R8LAY_P', '4096'))
X = os.environ.get('R8LAY_PREFIX', '').split()
ARCH = platform.machine()
ARM = ARCH in ('aarch64', 'arm64')

# name: (compiler, optimization flags)
CFGS = {
    'g2': ('gcc', '-O2'), 'g3': ('gcc', '-O3'), 'gs': ('gcc', '-Os'), 'g1': ('gcc', '-O1'),
    'g2p': ('gcc', '-O2 -fPIC'), 'g3p': ('gcc', '-O3 -fPIC'), 'gsp': ('gcc', '-Os -fPIC'),
    'c2': ('clang', '-O2'), 'c3': ('clang', '-O3'), 'cs': ('clang', '-Os'), 'c1': ('clang', '-O1'),
    'c2p': ('clang', '-O2 -fPIC'), 'c3p': ('clang', '-O3 -fPIC'), 'csp': ('clang', '-Os -fPIC'),
}


def roots():
    if not ROOT:
        sys.exit('set R8LAY_ROOT')
    return ROOT + '-o', ROOT + '-b'


def tree_src(t):
    if t.startswith('/'):
        return t, re.sub(r'-src$', '', os.path.basename(t.rstrip('/')))
    if not SRCROOT:
        sys.exit('set R8LAY_SRC (or pass absolute tree paths)')
    return os.path.join(SRCROOT, '%s-src' % t), t


def cflags(cfg):
    cc, opt = CFGS[cfg]
    cc = os.environ.get('R8LAY_CC_' + cc.upper(), cc)
    extra = os.environ.get('R8LAY_CFLAGS', '').split()
    f = opt.split() + ['-g', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-fvisibility=hidden']
    return cc.split(), f + extra


def ldflags(cfg):
    if ARM:
        # GNU ld has no --no-fix-cortex-a53-843419; gcc's driver adds the fix
        # by default on Debian/Ubuntu and drops it with -mno-fix-...; clang's
        # driver only adds it for Android
        fix = ['-mno-fix-cortex-a53-843419'] if CFGS[cfg][0] == 'gcc' else []
        return fix + ['-Wl,-z,separate-code']
    return []


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit('FAILED: %s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return r.stdout


def lib_files(src):
    fs = []
    for sub in ['lib', 'lib/x86', 'lib/arm']:
        d = os.path.join(src, sub)
        fs += sorted(os.path.join(d, f) for f in os.listdir(d) if f.endswith('.c'))
    return fs


def objname(f):
    return '%s_%s.o' % (os.path.basename(os.path.dirname(f)), os.path.basename(f)[:-2])


def run_jobs(jobs):
    """Run the commands with at most nproc at a time; exit on failure."""
    n = max(1, len(os.sched_getaffinity(0)))
    pend = list(jobs)
    act = []
    while pend or act:
        while pend and len(act) < n:
            j = pend.pop(0)
            act.append((j, subprocess.Popen(X + j, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)))
        j, p = act.pop(0)
        out = p.communicate()[0]
        if p.returncode:
            sys.exit('FAILED: %s\n%s' % (' '.join(j), out))
        if out.strip():
            sys.stderr.write(out)


def cmd_obj(cfg, trees):
    OBJ, _ = roots()
    cc, fl = cflags(cfg)
    for t in trees:
        src, name = tree_src(t)
        od = os.path.join(OBJ, cfg, name)
        if os.path.exists(os.path.join(od, 'DONE')) and '-f' not in sys.argv:
            continue
        shutil.rmtree(od, ignore_errors=True)
        os.makedirs(od)
        inc = ['-I' + src]
        jobs = []
        for f in lib_files(src):
            o = os.path.join(od, objname(f))
            if objname(f) == 'lib_deflate_compress.o':
                jobs.append(cc + fl + inc + ['-ffunction-sections', '-c', f, '-o', o])
                jobs.append(cc + fl + inc + ['-c', f, '-o', o[:-2] + '.plain.o'])
            else:
                jobs.append(cc + fl + inc + ['-c', f, '-o', o])
        jobs.append(cc + fl + ['-I' + src, '-c', LDBENCH, '-o', os.path.join(od, 'drv.o')])
        run_jobs(jobs)
        open(os.path.join(od, 'DONE'), 'w').write(' '.join(cc + fl) + '\n')
        print('obj %s %s' % (cfg, od))


def cmd_fscheck(cfg, trees):
    """-ffunction-sections must leave the compiler's assembly unchanged
    apart from section directives."""
    OBJ, _ = roots()
    cc, fl = cflags(cfg)
    fl = [f for f in fl if f != '-g']  # debug info refers to the sections
    ok = True

    def filt(path):
        keep = []
        for line in open(path):
            if re.match(r'^\s*\.(text|section|previous|pushsection|popsection|file|ident)\b', line):
                continue
            keep.append(line)
        return keep
    for t in trees:
        src, name = tree_src(t)
        d = os.path.join(OBJ, cfg, name)
        os.makedirs(d, exist_ok=True)
        f = os.path.join(src, 'lib', 'deflate_compress.c')
        a, b = os.path.join(d, 'fs_a.s'), os.path.join(d, 'fs_b.s')
        run(X + cc + fl + ['-I' + src, '-S', f, '-o', a])
        run(X + cc + fl + ['-I' + src, '-ffunction-sections', '-S', f, '-o', b])
        fa, fb = filt(a), filt(b)
        same = fa == fb
        ok = ok and same
        print('fscheck %s %s: %s (%d lines)' % (cfg, name, 'SAME' if same else 'DIFFERENT', len(fa)))
        if not same:
            n = 0
            for x, y in zip(fa, fb):
                if x != y:
                    print('    %s | %s' % (x.rstrip(), y.rstrip()))
                    n += 1
                    if n >= 10:
                        break
        os.unlink(a)
        os.unlink(b)
    print('FSCHECK ' + ('OK' if ok else 'FAILED'))
    return ok


SECRE = re.compile(r'^\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+(\S*)\s+(\d+)\s+(\d+)\s+(\d+)\s*$')


def sections(obj):
    """Executable, non-empty, non-COMDAT sections of obj, in file order."""
    res = []
    for line in run(['readelf', '-SW', obj]).splitlines():
        m = SECRE.match(line)
        if not m:
            continue
        name, typ, size, flg, al = m.group(1), m.group(2), int(m.group(5), 16), m.group(7), int(m.group(10))
        if typ != 'PROGBITS' or 'X' not in flg or 'G' in flg or size == 0:
            continue
        res.append({'sec': name, 'size': size, 'align': max(al, 1)})
    return res


def rnd(seed, kind, name):
    return int.from_bytes(hashlib.sha256(('%s:%s:%s' % (seed, kind, name)).encode()).digest()[:8], 'little')


def secprio(name):
    for i, p in enumerate(['.text.unlikely', '.text.exit', '.text.startup', '.text.hot']):
        if name == p or name.startswith(p + '.'):
            return i
    return 4


def plan(secs, seed, mode):
    """Returns (entry offset, [(section, pad, rel addr, size)])."""
    if 'shuf' in mode:
        order = sorted(secs, key=lambda s: rnd(seed, 'ord', s['sec']))
    else:
        order = sorted(secs, key=lambda s: secprio(s['sec']))  # stable
    if mode.startswith('anchor'):
        entry = P * (rnd(seed, 'entry', '') % 16)
    else:
        entry = rnd(seed, 'entry', '') % P
    addr = entry
    out = []
    for s in order:
        a = s['align']
        if mode.startswith('anchor'):
            tgt = (rnd(seed, 'tgt', s['sec']) % (P // a)) * a if a <= P else 0
            pad = (tgt - addr) % P
        else:
            pad = (rnd(seed, 'pad', s['sec']) % (P // a)) * a if a <= P else 0
        addr += pad
        addr = (addr + a - 1) // a * a
        out.append((s['sec'], pad, addr, s['size']))
        addr += s['size']
    return entry, out


def symname(sec):
    for p in ['.text.unlikely.', '.text.hot.', '.text.startup.', '.text.exit.']:
        if sec.startswith(p):
            return sec[len(p):] + ('.cold' if p == '.text.unlikely.' else '')
    return sec[len('.text.'):] if sec.startswith('.text.') else sec


def nm_syms(binary, types='tTwW'):
    syms = {}
    for line in run(['nm', '-S', binary]).splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in types:
            syms.setdefault(p[3], (int(p[0], 16), int(p[1], 16)))
    return syms


def link(cfg, od, outdir, seed, mode, exe):
    cc, fl = cflags(cfg)
    names = sorted(f for f in os.listdir(od) if f.endswith('.o') and f != 'drv.o' and not f.endswith('.plain.o'))
    # build.sh order: lib/, lib/x86/, lib/arm/
    names.sort(key=lambda f: (0 if f.startswith('lib_') else 1 if f.startswith('x86_') else 2, f))
    os.makedirs(outdir, exist_ok=True)
    out = os.path.join(outdir, exe)
    # The same inputs in the same order as build.sh (ldbench.o + an archive),
    # so that everything but deflate_compress.o's code is laid out as there.
    # Only the deflate_compress.o member differs: plain or with function
    # sections.
    dcobj = os.path.join(od, 'lib_deflate_compress.plain.o' if seed == 'plain' else 'lib_deflate_compress.o')
    members = []
    for n in names:
        if n == 'lib_deflate_compress.o':
            shutil.copy(dcobj, os.path.join(outdir, n))
            members.append(os.path.join(outdir, n))
        else:
            members.append(os.path.join(od, n))
    script = None
    if seed != 'plain':
        secs = sections(dcobj)
        entry, pl = plan(secs, seed, mode)
        pat = '*libdeflate.a:lib_deflate_compress.o'
        lines = ['SECTIONS', '{', '  .text.r8lay : ALIGN(%d)' % max(P, 4096), '  {',
                 '    . = . + 0x%x;' % entry]
        for sec, pad, addr, size in pl:
            lines.append('    . = . + 0x%x;' % pad)
            lines.append('    %s(%s)' % (pat, sec))
        lines += ['  } =%s' % ('0x00000000' if ARM else '0xcccccccc'), '}', 'INSERT BEFORE .text;', '']
        script = os.path.join(outdir, 'lay.ld')
        open(script, 'w').write('\n'.join(lines))
    lf = ['-Wl,-S', '-o', out] + ldflags(cfg) + (['-Wl,-T,' + script] if script else [])
    ar = os.path.join(outdir, 'libdeflate.a')
    if os.path.exists(ar):
        os.unlink(ar)
    run(['ar', 'rcs', ar] + members)
    run(X + cc + fl + [os.path.join(od, 'drv.o'), ar] + lf)
    os.unlink(ar)
    os.unlink(os.path.join(outdir, 'lib_deflate_compress.o'))
    data = {k: v[0] for k, v in nm_syms(out, 'rRdDbB').items()}
    if seed == 'plain':
        json.dump({'seed': seed, 'mode': 'plain', 'syms': nm_syms(out), 'data': data},
                  open(os.path.join(outdir, 'map.json'), 'w'))
        return out
    # verify the layout
    hdr = run(['readelf', '-SW', out])
    m = re.search(r'\]\s+\.text\.r8lay\s+PROGBITS\s+([0-9a-f]+)', hdr)
    if not m:
        sys.exit('no .text.r8lay in ' + out)
    base = int(m.group(1), 16)
    syms = nm_syms(out)
    fmap = {}
    for sec, pad, addr, size in pl:
        sn = symname(sec)
        if sn not in syms:
            sys.exit('symbol %s (section %s) missing in %s' % (sn, sec, out))
        a, sz = syms[sn]
        if a != base + addr or sz > size:
            sys.exit('layout mismatch %s: %x/%x vs planned %x/%x in %s' % (sn, a, sz, base + addr, size, out))
        fmap[sn] = [a, sz, pad]
    json.dump({'seed': seed, 'mode': mode, 'P': P, 'base': base, 'entry': entry, 'funcs': fmap,
               'syms': syms, 'data': data}, open(os.path.join(outdir, 'map.json'), 'w'))
    return out


def parse_seeds(s):
    if s == 'plain':
        return ['plain']
    res = []
    for part in s.split(','):
        if '-' in part:
            a, b = part.split('-')
            res += list(range(int(a), int(b) + 1))
        else:
            res.append(int(part))
    return res


def seed_dir(cfg, mode, name, s):
    _, BIN = roots()
    if s == 'plain':
        return os.path.join(BIN, cfg + '.plain', name, 's000')
    return os.path.join(BIN, '%s.%s' % (cfg, mode), name, 's%03d' % s)


def cmd_link(cfg, mode, seeds, trees):
    OBJ, _ = roots()
    seeds = parse_seeds(seeds)
    n = 0
    for t in trees:
        src, name = tree_src(t)
        od = os.path.join(OBJ, cfg, name)
        if not os.path.exists(os.path.join(od, 'DONE')):
            sys.exit('objects missing: ' + od)
        for s in seeds:
            link(cfg, od, seed_dir(cfg, mode, name, s), s, mode, 'ldbench')
            n += 1
    print('linked %d binaries (%s %s %s)' % (n, cfg, mode, ' '.join(trees)))


FUNC = re.compile(r'^([0-9a-f]+) <(.*)>:$')
INS = re.compile(r'^\s*([0-9a-f]+):\s+(.*)$')
TGT = re.compile(r'\b([0-9a-f]+) <([^>]*)>')


def dis(binary, names):
    """Per function: every instruction as 'offset-in-function text', with
    intra-function targets as L+offset, other code targets by symbol, and
    (second list) all remaining addresses and immediates >= 0x10 masked."""
    sizes = {k: v[1] for k, v in nm_syms(binary).items()}
    out = run(['objdump', '-d', '--no-show-raw-insn', '-w', binary])
    funcs = {}
    cur = None
    for line in out.splitlines():
        m = FUNC.match(line)
        if m:
            cur = m.group(2) if m.group(2) in names else None
            if cur:
                funcs[cur] = {'start': int(m.group(1), 16), 'ins': [], 'msk': []}
            continue
        if not cur:
            continue
        m = INS.match(line)
        if not m:
            continue
        a = int(m.group(1), 16)
        st = funcs[cur]['start']
        if a - st >= sizes.get(cur, 1 << 40):
            continue  # padding after the function
        txt = re.sub(r'\s+(#|//).*$', '', m.group(2))

        def tgt(mm):
            v = int(mm.group(1), 16)
            sym = mm.group(2)
            # inside this function: a branch target.  (An address past the
            # function's end that objdump still names after it is something
            # else, e.g. the page of the next function for an adrp.)
            if (sym == cur or sym.startswith(cur + '+')) and 0 <= v - st < sizes.get(cur, 0):
                return 'L+%x' % (v - st)
            return '<' + re.sub(r'\+0x[0-9a-f]+$', '+N', sym) + '>'
        if txt.split(None, 1)[0] in ('adrp', 'adr'):
            # the address (page) of some symbol, e.g. of another function
            # whose address is taken; it can even lie inside this function
            txt = TGT.sub('<ADDR>', txt)
        txt = re.sub(r'\s+', ' ', TGT.sub(tgt, txt)).strip()
        funcs[cur]['ins'].append('%x %s' % (a - st, txt))
        # masked form: symbols and every number gone; what is left is the
        # instruction sequence, each instruction's offset, the registers and
        # the branch targets inside the function
        mn, _, ops = txt.partition(' ')
        ops = re.sub(r'<[^>]*>', 'S', ops)
        ops = re.sub(r'(?<![\w.+])-?(?:0x)?[0-9a-f]+\b', 'N', ops)
        funcs[cur]['msk'].append('%x %s %s' % (a - st, mn, ops))
    return funcs


def cmd_check(cfg, mode, seeds, trees):
    """Across the given seeds of each tree: every deflate_compress.c function
    must have the same instructions at the same offsets (addresses masked),
    and every data symbol the same address mod 4096.  Differences from the
    plain link, if it was built, are reported but do not fail the check."""
    seeds = parse_seeds(seeds)
    ok = True
    datamod = {}
    for t in trees:
        _, name = tree_src(t)
        dirs = [seed_dir(cfg, mode, name, s) for s in seeds]
        maps = [json.load(open(os.path.join(d, 'map.json'))) for d in dirs]
        names = set(maps[0]['funcs'])
        ref = dis(os.path.join(dirs[0], 'ldbench'), names)
        nexact = nmask = 0
        bad = []
        plain = []
        bins = [os.path.join(d, 'ldbench') for d in dirs[1:]]
        pl = os.path.join(seed_dir(cfg, mode, name, 'plain'), 'ldbench')
        for b in bins + ([pl] if os.path.exists(pl) else []):
            of = dis(b, names)
            for n in sorted(names):
                if n not in of or n not in ref:
                    (plain if b == pl else bad).append('%s missing' % n)
                elif ref[n]['ins'] == of[n]['ins']:
                    nexact += b != pl
                elif ref[n]['msk'] == of[n]['msk']:
                    nmask += b != pl
                else:
                    msg = '%s: length %d vs %d' % (n, len(ref[n]['ins']), len(of[n]['ins']))
                    for x, y in zip(ref[n]['msk'], of[n]['msk']):
                        if x != y:
                            msg = '%s in %s: %s | %s' % (n, os.path.basename(os.path.dirname(b)), x, y)
                            break
                    (plain if b == pl else bad).append(msg)
        dm = {}
        for m in maps:
            for k, v in m.get('data', {}).items():
                dm.setdefault(k, set()).add(v % 4096)
        moved = sorted(k for k, v in dm.items() if len(v) > 1)
        datamod[name] = {k: min(v) for k, v in dm.items()}
        print('check %s.%s %s: %d functions x %d binaries: identical %d, address-only differences %d, '
              'CODE DIFFERENCES %d; vs the plain link: %s; data symbols %d, not fixed mod 4096 '
              'across seeds: %d%s' % (
                  cfg, mode, name, len(names), len(bins), nexact, nmask, len(bad),
                  ('%d functions differ' % len(plain)) if os.path.exists(pl) else 'not built',
                  len(dm), len(moved), (' (' + ' '.join(moved[:6]) + ')') if moved else ''))
        for b in bad[:8]:
            print('    ' + b)
        for b in plain[:4]:
            print('    plain: ' + b)
        if bad or moved:
            ok = False
    # data symbols whose address mod 4096 differs between trees (reported
    # only: a change that adds or resizes static data moves what follows)
    tn = list(datamod)
    for n in tn[1:]:
        both = [k for k in datamod[tn[0]] if k in datamod[n]]
        diff = [k for k in both if datamod[tn[0]][k] != datamod[n][k]]
        print('check %s.%s data mod 4096, %s vs %s: %d symbols in both, %d differ%s' % (
            cfg, mode, n, tn[0], len(both), len(diff), (' (' + ' '.join(diff[:6]) + ')') if diff else ''))
    print('CHECK ' + ('OK' if ok else 'FAILED'))
    return ok


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '-f']
    if len(args) < 2:
        sys.exit(__doc__)
    if args[0] == 'obj':
        cmd_obj(args[1], args[2:])
    elif args[0] == 'link':
        cmd_link(args[1], args[2], args[3], args[4:])
    elif args[0] == 'fscheck':
        sys.exit(0 if cmd_fscheck(args[1], args[2:]) else 3)
    elif args[0] == 'check':
        sys.exit(0 if cmd_check(args[1], args[2], args[3], args[4:]) else 3)
    else:
        sys.exit(__doc__)
