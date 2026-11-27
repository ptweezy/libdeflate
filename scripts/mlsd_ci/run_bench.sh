#!/bin/bash
# mlsd_ci run_bench.sh - test only: time master against this tree on a runner.
# usage: run_bench.sh CORPUSDIR OUTDIR
# Builds: master (aa5181e), new (this tree as it ships; the lanes are off on
# CPUs it doesn't list, so there it is a control for code placement), and
# builds with the lanes on any CPU (-DMLSD_TEST_ALWAYS_USABLE), see below.
set -euo pipefail
IN=${1:?}; OUT=${2:?}; CC=${CC:-cc}
HERE=$(cd "$(dirname "$0")" && pwd); TOP=$(cd "$HERE/../.." && pwd)
case $(uname -m) in x86_64|amd64) ARCH=x86 ;; *) ARCH=arm ;; esac
mkdir -p "$OUT"/{bin,streams,tools,res,src-master,in}
cp "$HERE/abn.py" "$OUT/tools/"
git -C "$TOP" archive aa5181e | tar -x -C "$OUT/src-master"
{
	uname -a
	if [ -r /proc/cpuinfo ]; then
		grep -m1 -E "model name|CPU part" /proc/cpuinfo || true
		grep -E "^(cpu family|model|stepping)\s" /proc/cpuinfo | sort -u | head -3 || true
		lscpu | grep -E "Model name|L1d|L2|Vendor|CPU\(s\):" || true
	else
		sysctl -n machdep.cpu.brand_string hw.l1dcachesize hw.ncpu || true
	fi
	$CC --version | head -1
} > "$OUT/res/system.txt" 2>&1
cat "$OUT/res/system.txt"
build() { # name srcdir flags...
	local name=$1 src=$2; shift 2
	$CC -O2 -DNDEBUG -std=gnu11 -I"$src" "$@" "$HERE/dbench.c" "$src"/lib/*.c "$src"/lib/$ARCH/*.c -o "$OUT/bin/dbench_$name"
}
# new = this tree as it ships (lanes on Zen 4, Zen 5 and Granite Rapids);
# f = the same with the lanes on any CPU.
build master "$OUT/src-master"
build new "$TOP"
build f "$TOP" -DMLSD_TEST_ALWAYS_USABLE
$CC -O2 -DNDEBUG -std=gnu11 -I"$TOP" -c "$TOP/lib/deflate_decompress.c" -o "$OUT/dd_new.o"
objdump -d --no-show-raw-insn -M intel "$OUT/dd_new.o" | awk '/<decompress_multilane_bmi2>:/{p=1} p&&/^$/{exit} p' > "$OUT/ml_new.s"
echo "new: $(python3 "$HERE/loop.py" "$OUT/ml_new.s" || true)" | tee -a "$OUT/res/loop.txt"
$CC -O2 -I"$TOP" "$HERE/mkstream.c" "$TOP"/lib/*.c "$TOP"/lib/$ARCH/*.c -o "$OUT/bin/mkstream"
SIL="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
for f in $SIL enwik8; do
	ln -sf "$IN/$f" "$OUT/in/$f"
	for l in 1 6; do "$OUT/bin/mkstream" $l 0 "$IN/$f" "$OUT/streams/$f.L$l.c0"; done
done
for f in $SIL; do "$OUT/bin/mkstream" 12 0 "$IN/$f" "$OUT/streams/$f.L12.c0"; done
cd "$OUT"
export BENCH_CPU=${BENCH_CPU:-2}
for l in 6 1; do
	echo "== L$l $(uptime)" | tee -a res/bench.txt
	python3 tools/abn.py -o res/L$l.json -r 5 -n 5 -ref master -b master,new,f -s .L$l.c0 in/* | tee -a res/bench.txt
done
echo "== L12 $(uptime)" | tee -a res/bench.txt
python3 tools/abn.py -o res/L12.json -r 5 -n 5 -ref master -b master,new,f -s .L12.c0 $(for f in $SIL; do echo in/$f; done) | tee -a res/bench.txt
