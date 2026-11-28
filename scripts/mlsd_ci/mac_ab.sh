#!/bin/bash
# mlsd_ci mac_ab.sh - test only: on an Apple arm64 Mac, time master and the
# lanes at two commits (default fb7ac92 = old, 4af792b = new: the step loop
# marked likely), each at 4 code placements (a pad of 0, 16, 32 or 48 bytes
# linked in front of deflate_decompress.o), levels 6, 1 and 12.
# usage: mac_ab.sh CORPUSDIR OUTDIR [OLD NEW]
# CORPUSDIR holds the 12 Silesia files and enwik8 (scripts/mlsd_ci/inputs.sh
# fetches them).  Run from a checkout that has aa5181e, OLD and NEW.  Takes
# ~/bench.lock while timing; prints the summary at the end (also OUTDIR/summary.txt).
set -euo pipefail
IN=${1:?}; OUT=${2:?}; OLD=${3:-fb7ac92}; NEW=${4:-4af792b}
HERE=$(cd "$(dirname "$0")" && pwd); TOP=$(cd "$HERE/../.." && pwd)
SIL="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
mkdir -p "$OUT"/{bin,streams,tools,res,in}
cp "$HERE/abn.py" "$OUT/tools/"
for v in master:aa5181e old:$OLD new:$NEW; do
	rm -rf "$OUT/src-${v%%:*}"; mkdir -p "$OUT/src-${v%%:*}"
	git -C "$TOP" archive ${v#*:} | tar -x -C "$OUT/src-${v%%:*}"
done
{ sysctl -n machdep.cpu.brand_string hw.ncpu; sw_vers -productVersion; cc --version | sed -n 1p; echo "old $OLD new $NEW"; } > "$OUT/res/system.txt"
cat "$OUT/res/system.txt"
build() { # name src pad
	local name=$1 src=$2 pad=$3 f srcs=
	printf '\t.text\n\t.p2align 4\n\t.globl _mlsd_ci_pad\n_mlsd_ci_pad:\n\t.space %d, 0\n' $pad > "$OUT/pad_$name.s"
	for f in "$src"/lib/*.c; do [ "${f##*/}" = deflate_decompress.c ] && srcs="$srcs $OUT/pad_$name.s"; srcs="$srcs $f"; done
	cc -O2 -DNDEBUG -std=gnu11 -arch arm64 -I"$src" "$HERE/dbench.c" $srcs "$src"/lib/arm/*.c -o "$OUT/bin/dbench_$name"
}
build master "$OUT/src-master" 0
B=master
for p in 0 16 32 48; do build old_p$p "$OUT/src-old" $p; build new_p$p "$OUT/src-new" $p; B=$B,old_p$p,new_p$p; done
cc -O2 -std=gnu11 -arch arm64 -I"$OUT/src-new" "$HERE/mkstream.c" "$OUT/src-new"/lib/*.c "$OUT/src-new"/lib/arm/*.c -o "$OUT/bin/mkstream"
for f in $SIL enwik8; do
	ln -sf "$IN/$f" "$OUT/in/$f"
	for l in 1 6 12; do [ -e "$OUT/streams/$f.L$l.c0" ] || "$OUT/bin/mkstream" $l 0 "$IN/$f" "$OUT/streams/$f.L$l.c0"; done
done
cd "$OUT"
until mkdir ~/bench.lock 2> /dev/null; do sleep 10; done
echo "mlsd_ci mac_ab.sh (lanes step-loop hint), $(date)" > ~/bench.lock/owner
trap 'rm -rf ~/bench.lock' EXIT
for l in L6 L1 L12; do
	files=in/*; [ $l = L12 ] && files=$(for f in $SIL; do echo in/$f; done)
	echo "== $l load $(sysctl -n vm.loadavg) $(date)" | tee -a res/bench.txt
	python3 tools/abn.py -o res/$l.json -r ${ROUNDS:-4} -n 5 -ref master -b $B -s .$l.c0 $files >> res/bench.txt 2>> res/err.txt
done
python3 - res <<'PY' | tee summary.txt
import json, math, os, statistics, sys
gml = lambda xs: math.exp(sum(math.log(x) for x in xs) / len(xs))
pc = lambda x: (x - 1) * 100
for l in ('L6', 'L1', 'L12'):
    t = json.load(open(os.path.join(sys.argv[1], l + '.json')))['times']; fs = list(t['master'])
    po = [b for b in t if b.startswith('old_p')]; pn = [b for b in t if b.startswith('new_p')]
    o = {f: gml([min(t[b][f]) for b in po]) for f in fs}; n = {f: gml([min(t[b][f]) for b in pn]) for f in fs}
    pr = [statistics.median(gml([t[a][f][i] for a in po]) / gml([t[b][f][i] for b in pn]) for i in range(len(t[po[0]][f]))) for f in fs]
    print('%-3s vs master: old %+.1f%% new %+.1f%%; new/old %+.2f%% (best over rounds), %+.2f%% (paired median); files %s' % (
        l, pc(gml([min(t['master'][f]) / o[f] for f in fs])), pc(gml([min(t['master'][f]) / n[f] for f in fs])),
        pc(gml([o[f] / n[f] for f in fs])), pc(gml(pr)), ' '.join('%s %+.1f' % (os.path.basename(f)[:7], pc(o[f] / n[f])) for f in fs)))
PY
