#!/bin/bash
# mlsd_ci run_ab.sh - test only: time master, old (the lanes at $OLD) and new
# (this tree) on a runner, and this tree with each scripts/mlsd_ci/variants/NAME.patch
# (build v_NAME).  Where the lanes are on as shipped at $OLD: levels
# 6, 1 and 12.  Elsewhere: old and new with the lanes forced on, level 6.
# usage: run_ab.sh CORPUSDIR OUTDIR
set -euo pipefail
IN=${1:?}; OUT=${2:?}; CC=${CC:-cc}; OLD=${OLD:-b2f1b4e}
HERE=$(cd "$(dirname "$0")" && pwd); TOP=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT"/{bin,streams,tools,res,src-master,src-old,in}
cp "$HERE/abn.py" "$OUT/tools/"
git -C "$TOP" archive aa5181e | tar -x -C "$OUT/src-master"
git -C "$TOP" archive $OLD | tar -x -C "$OUT/src-old"
{
	uname -a
	grep -m1 "model name" /proc/cpuinfo
	grep -E "^(vendor_id|cpu family|model|stepping)\s" /proc/cpuinfo | sort -u | head -4
	lscpu | grep -E "Model name|L1d|L2|CPU\(s\):" || true
	$CC --version | sed -n 1p
} > "$OUT/res/system.txt" 2>&1
cat "$OUT/res/system.txt"
# allow_mlsd() at b2f1b4e
ON=$(awk -F': ' '/^vendor_id/{v=$2} /^cpu family/{f=$2} /^model\t/{m=$2} END {
	on = (v == "AuthenticAMD" && (f >= 26 || (f == 25 && ((m >= 16 && m < 32) || (m >= 96 && m < 176))))) ||
	     (v == "GenuineIntel" && f == 6 && (m == 173 || m == 207)); print on ? 1 : 0 }' /proc/cpuinfo)
echo "lanes on as shipped: $ON" | tee -a "$OUT/res/system.txt"
build() { # name srcdir flags...
	local name=$1 src=$2; shift 2
	$CC -O2 -DNDEBUG -std=gnu11 -I"$src" "$@" "$HERE/dbench.c" "$src"/lib/*.c "$src"/lib/x86/*.c -o "$OUT/bin/dbench_$name"
	$CC -O2 -DNDEBUG -std=gnu11 -I"$src" "$@" -c "$src/lib/deflate_decompress.c" -o "$OUT/dd_$name.o"
	objdump -d --no-show-raw-insn -M intel "$OUT/dd_$name.o" | awk '/<decompress_multilane_bmi2>:/{p=1} p&&/^$/{exit} p' > "$OUT/ml_$name.s"
	echo "$name: $(python3 "$HERE/loop.py" "$OUT/ml_$name.s" 2>&1 | head -1)" | tee -a "$OUT/res/loop.txt"
}
F=; [ $ON = 1 ] || F=-DMLSD_TEST_ALWAYS_USABLE
build master "$OUT/src-master"
build old "$OUT/src-old" $F
build new "$TOP" $F
B=master,old,new
for v in $(ls "$HERE/variants" 2> /dev/null | sed -n 's/\.patch$//p'); do
	mkdir -p "$OUT/src-$v"
	git -C "$TOP" archive HEAD lib common_defs.h libdeflate.h | tar -x -C "$OUT/src-$v"
	patch -s -d "$OUT/src-$v" -p1 < "$HERE/variants/$v.patch"
	build v_$v "$OUT/src-$v" $F
	B=$B,v_$v
done
$CC -O2 -I"$TOP" "$HERE/mkstream.c" "$TOP"/lib/*.c "$TOP"/lib/x86/*.c -o "$OUT/bin/mkstream"
SIL="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
LV="6 1 12"; [ $ON = 1 ] || LV=6
for f in $SIL enwik8; do
	ln -sf "$IN/$f" "$OUT/in/$f"
	for l in $LV; do [ $l = 12 ] && [ $f = enwik8 ] || "$OUT/bin/mkstream" $l 0 "$IN/$f" "$OUT/streams/$f.L$l.c0"; done
done
cd "$OUT"
export BENCH_CPU=${BENCH_CPU:-2}
for l in $LV; do
	files=in/*; [ $l = 12 ] && files=$(for f in $SIL; do echo in/$f; done)
	echo "== L$l $(uptime)" | tee -a res/bench.txt
	python3 tools/abn.py -o res/L$l.json -r ${ROUNDS:-5} -n 5 -ref master -b $B -s .L$l.c0 $files | tee -a res/bench.txt
done
