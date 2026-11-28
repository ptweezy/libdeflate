#!/bin/bash
# mlsd_ci run_prof.sh - test only: per-block time with the lanes off and on.
# usage: run_prof.sh CORPUSDIR OUTDIR
# One binary (x86_64 with 6 lanes, lanes on any CPU); see prof.c for the modes.
set -euo pipefail
IN=${1:?}; OUT=${2:?}; CC=${CC:-cc}
HERE=$(cd "$(dirname "$0")" && pwd); TOP=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT"/{bin,streams,res,src-prof}
{
	uname -a
	grep -m1 "model name" /proc/cpuinfo || true
	grep -E "^(cpu family|model|stepping)\s" /proc/cpuinfo | sort -u | head -3 || true
	lscpu | grep -E "Model name|L1d|L2|Vendor|CPU\(s\):" || true
	$CC --version | head -1
} > "$OUT/res/system.txt" 2>&1
cat "$OUT/res/system.txt"
cp -r "$TOP/lib" "$TOP/libdeflate.h" "$TOP/common_defs.h" "$OUT/src-prof/"
python3 "$HERE/prof_patch.py" "$OUT/src-prof"
P="$OUT/src-prof"
build() { local name=$1; shift; $CC -O2 -DNDEBUG -std=gnu11 -I"$P" -DMLSD_TEST_ALWAYS_USABLE "$@" "$HERE/prof.c" "$P"/lib/*.c "$P"/lib/x86/*.c -o "$OUT/bin/prof_$name"; }
build x -DMLSD_LANES=6 -DMLSD_INTERLEAVE=0 -DMLSD_GATE32=6 -DMLSD_GATE_LEN=18
$CC -O2 -I"$TOP" "$HERE/mkstream.c" "$TOP"/lib/*.c "$TOP"/lib/x86/*.c -o "$OUT/bin/mkstream"
SIL="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
for f in $SIL enwik8; do
	for l in 1 6; do "$OUT/bin/mkstream" $l 0 "$IN/$f" "$OUT/streams/$f.L$l.c0"; done
done
for f in $SIL; do "$OUT/bin/mkstream" 12 0 "$IN/$f" "$OUT/streams/$f.L12.c0"; done
cd "$OUT"
for L in L6 L12 L1; do
	echo "== $L $(uptime)"
	for s in streams/*.$L.c0; do
		f=$(basename "$s" .$L.c0)
		taskset -c ${BENCH_CPU:-2} bin/prof_x -n 15 "$s" res/$f.$L.csv res/$f.$L.lens
	done
done
echo "== done $(uptime)"
