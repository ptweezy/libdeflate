#!/bin/bash
# mlsd_ci desc_mac.sh - test only: on an Apple arm64 Mac, time master (aa5181e)
# and NEW (default fb7ac92), each at 4 code placements (0, 16, 32 and 48 bytes
# of padding in front of deflate_decompress.o), at levels 6, 1 and 12 (Silesia +
# enwik8) and at level 6 in 1 MiB streams.  MINER=1 pauses xmrig while timing.
# usage: desc_mac.sh CORPUSDIR OUTDIR [NEW]
# Run from a checkout that has aa5181e and NEW.  Takes ~/bench.lock while
# timing; prints the summary at the end (also OUTDIR/summary.txt).
set -euo pipefail
IN=${1:?}; OUT=${2:?}; NEW=${3:-fb7ac92}
HERE=$(cd "$(dirname "$0")" && pwd); TOP=$(cd "$HERE/../.." && pwd)
SIL="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
mkdir -p "$OUT"/{bin,streams,tools,res,in}
cp "$HERE/abn.py" "$OUT/tools/"
for v in master:aa5181e new:$NEW; do
	rm -rf "$OUT/src-${v%%:*}"; mkdir -p "$OUT/src-${v%%:*}"
	git -C "$TOP" archive ${v#*:} | tar -x -C "$OUT/src-${v%%:*}"
done
{ sysctl -n machdep.cpu.brand_string hw.ncpu; sw_vers -productVersion; cc --version | sed -n 1p; echo "new $NEW"; } > "$OUT/res/system.txt"
cat "$OUT/res/system.txt"
build() { # name src pad
	local name=$1 src=$2 pad=$3 f srcs=
	printf '\t.text\n\t.p2align 4\n\t.globl _mlsd_ci_pad\n_mlsd_ci_pad:\n\t.space %d, 0\n' $pad > "$OUT/pad_$name.s"
	for f in "$src"/lib/*.c; do [ "${f##*/}" = deflate_decompress.c ] && srcs="$srcs $OUT/pad_$name.s"; srcs="$srcs $f"; done
	cc -O2 -DNDEBUG -std=gnu11 -arch arm64 -I"$src" "$HERE/dbench.c" $srcs "$src"/lib/arm/*.c -o "$OUT/bin/dbench_$name"
}
B=
for p in 0 16 32 48; do build master_p$p "$OUT/src-master" $p; build new_p$p "$OUT/src-new" $p; B=$B,master_p$p,new_p$p; done
B=${B#,}
cc -O2 -std=gnu11 -arch arm64 -I"$OUT/src-new" "$HERE/mkstream.c" "$OUT/src-new"/lib/*.c "$OUT/src-new"/lib/arm/*.c -o "$OUT/bin/mkstream"
for f in $SIL enwik8; do
	ln -sf "$IN/$f" "$OUT/in/$f"
	for l in 1 6 12; do [ -e "$OUT/streams/$f.L$l.c0" ] || "$OUT/bin/mkstream" $l 0 "$IN/$f" "$OUT/streams/$f.L$l.c0"; done
	[ -e "$OUT/streams/$f.L6.c1048576" ] || "$OUT/bin/mkstream" 6 1048576 "$IN/$f" "$OUT/streams/$f.L6.c1048576"
done
cd "$OUT"
until mkdir ~/bench.lock 2> /dev/null; do sleep 10; done
echo "mlsd_ci desc_mac.sh (PR 488 description, master vs $NEW, ~25 min), $(date)" > ~/bench.lock/owner
miner=
cleanup() { [ -n "$miner" ] && kill -CONT $miner 2> /dev/null; rm -rf ~/bench.lock; }
trap cleanup EXIT
if [ "${MINER:-}" = 1 ]; then miner=$(pgrep -x xmrig || true); [ -n "$miner" ] && kill -STOP $miner && sleep 60; fi
for s in L6.c0 L1.c0 L12.c0 L6.c1048576; do
	echo "== $s load $(sysctl -n vm.loadavg) $(date)" | tee -a res/bench.txt
	python3 tools/abn.py -o res/$s.json -r ${ROUNDS:-4} -n 5 -ref master_p0 -b $B -s .$s in/* >> res/bench.txt 2>> res/err.txt
done
python3 - res <<'PY' | tee summary.txt
import json, math, os, sys
gml = lambda xs: math.exp(sum(math.log(x) for x in xs) / len(xs))
for s in ('L1.c0', 'L6.c0', 'L12.c0', 'L6.c1048576'):
    t = json.load(open(os.path.join(sys.argv[1], s + '.json')))['times']; fs = list(t['master_p0'])
    r = {}
    for f in fs:
        m = gml([min(t[b][f]) for b in t if b.startswith('master_p')]); n = gml([min(t[b][f]) for b in t if b.startswith('new_p')])
        r[os.path.basename(f)] = m / n
    w = min(r.items(), key=lambda kv: kv[1])
    print('%-12s new vs master %+.1f%%, lowest %+.1f%% (%s); files %s' % (s, (gml(list(r.values())) - 1) * 100, (w[1] - 1) * 100, w[0],
          ' '.join('%s %+.1f' % (f[:7], (v - 1) * 100) for f, v in r.items())))
PY
