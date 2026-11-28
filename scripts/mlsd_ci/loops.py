#!/usr/bin/env python3
"""loops.py FILE.s [-v]: every loop (backward jump) in an objdump -d -M intel listing of one function:
range, instructions, stack references, loads, stores; innermost loops first by address."""
import re, sys
lines = [l for l in open(sys.argv[1]).read().splitlines() if re.match(r'\s*[0-9a-f]+:\s', l)]
addr = [int(l.split(':')[0], 16) for l in lines]
idx = {a: i for i, a in enumerate(addr)}
loops = []
for j, l in enumerate(lines):
    m = re.search(r'\bj\w+\s+([0-9a-f]+) <', l)
    if m:
        t = int(m.group(1), 16)
        if t <= addr[j] and t in idx:
            loops.append((idx[t], j))
LD = re.compile(r'\b(mov|movzx|movsx|movsxd|shrx|shlx|add|sub|and|or|xor|cmp|test|movdqu|movups|vmovdqu|vmovups|lea)\s+\w+,\s*(XMMWORD |YMMWORD |QWORD |DWORD |WORD |BYTE )?PTR')
ST = re.compile(r'\b(mov|movdqu|movups|vmovdqu|vmovups|add|sub|or|and)\s+(XMMWORD |YMMWORD |QWORD |DWORD |WORD |BYTE )PTR')
for i, j in sorted(set(loops), key=lambda x: (x[1] - x[0], x[0])):
    body = lines[i:j + 1]
    nld = sum(bool(LD.search(b)) and 'lea' not in b for b in body)
    nst = sum(bool(ST.search(b)) for b in body)
    print('loop %x..%x: %d insns, %d stack refs, %d loads, %d stores' % (addr[i], addr[j], len(body),
          sum('rsp' in b or 'rbp-' in b for b in body), nld, nst))
    if '-v' in sys.argv:
        print('\n'.join('   ' + b.strip()[:100] for b in body))
