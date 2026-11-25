#!/usr/bin/env python3
"""nooutline_ci loopinfo.py - test-only: where a function's main loop sits.

usage: otool -arch arm64 -tV -p _FUNC LIB | loopinfo.py FUNC
       loopinfo.py --code FUNC < disassembly    (instructions only, for diffing)

Prints "start=0x... head=+N" where head is the offset within a 128-byte line
of the branch target that the most backward branches of the function go to
(its main loop head).  With --code, prints the function's instructions with
addresses and branch/page targets removed, so that two placements of the
same code compare equal.
"""
import re
import sys
from collections import Counter

code_only = sys.argv[1] == '--code'
func = sys.argv[-1].lstrip('_')
insns = []
inside = False
# Read everything (otool -p disassembles to the end of the section; stopping
# early would kill it with SIGPIPE).
for line in sys.stdin.read().split('\n'):
    if re.match(r'^_?%s:' % re.escape(func), line):
        inside = True
        continue
    if inside and re.match(r'^_\w', line):
        break
    m = re.match(r'^([0-9a-f]{8,16})\t(.*)$', line)
    if inside and m:
        insns.append((int(m.group(1), 16), m.group(2)))
if not insns:
    sys.exit('loopinfo: function %s not found' % func)
if code_only:
    # Alignment padding after the function shows up as trailing nops (from
    # the assembler) and zero words (from the linker, shown as udf).
    while insns and insns[-1][1].split()[0] in ('nop', 'udf'):
        insns.pop()
    for _, ins in insns:
        ins = re.sub(r';.*$', '', ins)
        ins = re.sub(r'0x[0-9a-f]+', 'X', ins)
        ins = re.sub(r'^(adrp\s+\w+,)\s*\S+', r'\1 P', ins)
        print(ins.strip())
    sys.exit(0)
start = insns[0][0]
targets = Counter()
for a, ins in insns:
    op = ins.split()[0]
    m = re.search(r'0x([0-9a-f]+)\s*$', ins)
    if m and re.match(r'^(b(\.\w+)?|cbn?z|tbn?z)$', op):
        t = int(m.group(1), 16)
        if start <= t <= a:
            targets[t] += 1
head = targets.most_common(1)[0][0] if targets else start
print('start=0x%x(+%d) head=+%d' % (start, start % 128, head % 128))
