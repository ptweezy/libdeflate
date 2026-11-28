import sys, re
# print the hot step loop: from the branch target just before the first 'and ...,0xfff' to the back-edge jumping there
lines = open(sys.argv[1]).read().splitlines()
addr = lambda l: int(l.split(':')[0].strip(), 16)
first = next(i for i, l in enumerate(lines) if re.search(r'\band\s+\w+,0xfff$', l))
# back-edges: jumps to a target at or before 'first', within 40 lines before it
best = None
for j in range(first, min(len(lines), first + 400)):
    m = re.search(r'\bj\w+\s+([0-9a-f]+) <', lines[j])
    if m:
        tgt = int(m.group(1), 16)
        for i in range(max(0, first - 40), first + 1):
            try:
                if addr(lines[i]) == tgt:
                    best = (i, j); break
            except ValueError:
                pass
        if best and best[1] == j:
            break
i, j = best
body = lines[i:j + 1]
print("loop %s..%s: %d instructions, %d stack refs, %d loads, %d stores" % (lines[i].split(':')[0].strip(), lines[j].split(':')[0].strip(), len(body),
      sum('rsp' in l for l in body), sum(bool(re.search(r'(mov|movzx|shrx|add|and|or|cmp)\s+\w+,(QWORD|DWORD|WORD|BYTE) PTR', l)) for l in body),
      sum(bool(re.search(r'mov\s+(QWORD|DWORD|WORD|BYTE) PTR', l)) for l in body)))
if len(sys.argv) > 2:
    print('\n'.join(l[:90] for l in body))
