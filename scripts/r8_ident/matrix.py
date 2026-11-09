#!/usr/bin/env python3
"""Print the job matrix of the r8-ident workflow as "matrix=<JSON>".

usage: matrix.py BRANCH [ONLY [COMMIT_MESSAGE]]

ONLY is a regular expression that selects job names; a line "r8-only: REGEX"
in COMMIT_MESSAGE does the same (workflow_dispatch isn't available while the
workflow isn't on the default branch).  Without either, every job runs,
except on a branch whose name contains "neg" or "quick", where only the jobs
marked quick run (one or two per kind of platform).

Each job has:
  name      PLATFORM or PLATFORM.SHARD; the levels of the slow platforms
            (the emulated ones and 32-bit MSVC) are split into three shards
            that take about the same time
  os        the runner
  apt       packages to install (Linux)
  vcarch    the MSVC target architecture (Windows)
  cc, cflags, run, levels, unit
            R8_CC, R8_CFLAGS, R8_RUN, R8_LEVELS and R8_UNIT of run.sh

The order is the priority: big-endian first.
"""
import json
import re
import sys

ALL = '0,1,2,3 4,5,6,7 8,9 10 11 12'
# (shard, levels, unit tests)
SHARDS = (('L0-7', '0,1,2,3 4,5,6,7', 1), ('L8-11', '8,9 10 11', 0),
          ('L12', '12', 0))
jobs = []


def job(name, os, cc, cflags='-O2', apt='', run='', vcarch='', levels=ALL,
        unit=1, quick=False):
    jobs.append(dict(name=name, platform=name.split('.')[0], os=os, cc=cc,
                     cflags=cflags, apt=apt, run=run, vcarch=vcarch,
                     levels=levels, unit=unit, quick=quick))


def emulated(platform, triplet, run, quick=False, os='ubuntu-24.04',
             cflags='-O2 -static'):
    for shard, levels, unit in SHARDS:
        job(platform + '.' + shard, os, triplet + '-gcc', cflags,
            apt='qemu-user-static gcc-' + triplet, run=run, levels=levels,
            unit=unit, quick=quick and unit == 1)


# 1. Big-endian, under qemu-user.  ppc64 and ppc have
#    UNALIGNED_ACCESS_IS_FAST, s390x doesn't.
emulated('s390x-qemu', 's390x-linux-gnu', 'qemu-s390x-static -cpu max',
         quick=True)
emulated('ppc64-qemu', 'powerpc64-linux-gnu', 'qemu-ppc64-static', quick=True)
emulated('ppc-qemu', 'powerpc-linux-gnu', 'qemu-ppc-static')

# 2. ARM, native.  The 32-bit binaries run natively if the runner's CPU and
#    kernel support AArch32, else under qemu-user.
ARM = 'ubuntu-24.04-arm'
job('aarch64-gcc', ARM, 'gcc', quick=True)
job('aarch64-clang', ARM, 'clang')
for name, flags in (('armhf-gcc', ''), ('armhf-neon-gcc', ' -mfpu=neon')):
    job(name, ARM, 'arm-linux-gnueabihf-gcc', '-O2 -static' + flags,
        apt='gcc-arm-linux-gnueabihf', run='auto:qemu-arm-static')

# 3. MSVC.  The 32-bit code is several times slower at the high levels.
for arch, os in (('x64', 'windows-2025'), ('x86', 'windows-2025'),
                 ('arm64', 'windows-11-arm')):
    for opt in ('O2', 'O1'):
        name = 'msvc-%s-%s' % (arch, opt)
        if arch == 'x86':
            for shard, levels, unit in SHARDS:
                job(name + '.' + shard, os, 'cl', '-' + opt, vcarch=arch,
                    levels=levels, unit=unit)
        else:
            job(name, os, 'cl', '-' + opt, vcarch=arch,
                quick=(arch, opt) == ('x64', 'O2'))

# 4. macOS arm64, Apple clang.
job('macos-arm64-clang', 'macos-15', 'clang', quick=True)

# 5. x86 Linux (control), and riscv64 under qemu-user.
X86 = 'ubuntu-24.04'
job('x86_64-gcc', X86, 'gcc', quick=True)
job('x86_64-clang', X86, 'clang')
job('i686-gcc', X86, 'gcc', '-O2 -m32', apt='gcc-multilib')
job('i686-clang', X86, 'clang', '-O2 -m32', apt='gcc-multilib')
emulated('riscv64-qemu', 'riscv64-linux-gnu', 'qemu-riscv64-static')

branch = sys.argv[1] if len(sys.argv) > 1 else ''
only = sys.argv[2] if len(sys.argv) > 2 else ''
if not only and len(sys.argv) > 3:
    m = re.search(r'^r8-only: *(\S+)', sys.argv[3], re.M)
    only = m.group(1) if m else ''
if only:
    jobs = [j for j in jobs if re.search(only, j['name'])]
elif 'neg' in branch or 'quick' in branch:
    jobs = [j for j in jobs if j['quick']]
for j in jobs:
    del j['quick']
if not jobs:
    sys.exit('no jobs selected')
print('matrix=' + json.dumps({'include': jobs}, separators=(',', ':')))
