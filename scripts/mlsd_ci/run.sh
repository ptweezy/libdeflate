#!/bin/bash
# mlsd_ci run.sh - test-only: interleaved A/B timing of decompression for the
# commit series below HEAD, plus a differential fuzz of the multi-lane
# decoder against the base.  Not for upstream.
# usage: run.sh INDIR WORKDIR
#
# Trees: tree 0 = HEAD~(N+1) (the base), tree 1 = HEAD~N, ..., tree N = HEAD~1;
# HEAD is the test-only commit.  N is $AB_SERIES (default 3).  Each tree is
# built into bench.c with $CC (default cc).  Times are user-mode cycles when
# the runner exposes a PMU (BENCH_PMU, probed below), else wall-clock ns.
# "d min%" compares the best time of each side; "d med%" is the median of the
# paired per-round ratios.  Decompressed output is verified on every run.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
IN=${1:?usage: run.sh INDIR WORKDIR}
W=${2:?usage: run.sh INDIR WORKDIR}
N=${AB_SERIES:-3}
CC=${CC:-cc}
CPU=${AB_CPU:-2}
PY=${PY:-python3}
R=${AB_ROUNDS:-7}
R2=${AB_ROUNDS2:-5}
R3=${AB_ROUNDS3:-3}
FUZZ_ITERS=${FUZZ_ITERS:-150}
NAMES=${AB_FILES:-dickens mozilla mr nci ooffice osdb reymont samba sao webster xml x-ray enwik8}
SIL=${AB_SIL:-dickens mozilla mr nci ooffice osdb reymont samba sao webster xml x-ray}
FUZZ_FILES=${AB_FUZZ_FILES:-dickens mr ooffice samba xml x-ray sao}
mkdir -p "$W"
RES=$W/results.txt
: > "$RES"

{
	echo "== system"
	uname -srm
	lscpu 2>/dev/null | grep -E 'Model name|^CPU\(s\)|Thread|L1d|L2 |L3 ' || true
	sysctl -n machdep.cpu.brand_string hw.perflevel0.physicalcpu hw.perflevel0.l1dcachesize 2>/dev/null | tr '\n' ' ' || true; echo
	echo "flags: $(grep -o -w -E 'avx512f|avx512bw|avx512vl|bmi2|avx2|asimd' /proc/cpuinfo 2>/dev/null | sort -u | tr '\n' ' ')"
	$CC --version | head -1
	# macOS: the QoS class and darwinbg state the benchmark inherits (they
	# decide whether QoS-gated code paths run at all)
	if [ "$(uname)" = Darwin ]; then
		printf '%s\n' '#include <stdio.h>' '#include <pthread/qos.h>' '#include <sys/resource.h>' \
			'int main(void) { printf("thread QoS class 0x%x, PRIO_DARWIN_PROCESS %d\n", (unsigned)qos_class_self(), getpriority(PRIO_DARWIN_PROCESS, 0)); return 0; }' > "$W/qos.c"
		$CC -o "$W/qos" "$W/qos.c" && "$W/qos" || true
		sysctl -n hw.nperflevels hw.perflevel1.physicalcpu 2>/dev/null | tr '\n' ' ' || true; echo
	fi
} | tee -a "$RES"

for i in $(seq 0 "$N"); do
	rev="HEAD~$((N + 1 - i))"
	mkdir -p "$W/t$i"
	git -C "$TOP" archive "$rev" | tar -x -C "$W/t$i"
	$CC -O2 -std=gnu11 -I"$W/t$i" -o "$W/b$i" "$HERE/bench.c" \
		"$W/t$i"/lib/*.c "$W/t$i"/lib/*/*.c
	echo "tree $i = $(git -C "$TOP" log -1 --format='%h %s' "$rev")" | tee -a "$RES"
done
cp "$W/b0" "$W/b0copy"

# Use user-mode cycles if the PMU is usable here.
if [ -z "${BENCH_PMU+x}" ]; then
	head -c 100000 "$IN/dickens" > "$W/probe"
	if [ "$(uname)" = Linux ] &&
	   BENCH_PMU=1 "$W/b0" -l 1 -n 1 -C "$W/probe" > /dev/null 2>&1; then
		export BENCH_PMU=1
		echo "timing: user-mode cycles (PMU)" | tee -a "$RES"
	else
		echo "timing: wall-clock ns (no PMU)" | tee -a "$RES"
	fi
fi

# Diagnostic: size and stack traffic of the lane loop in each enabled tree.
if command -v objdump > /dev/null 2>&1; then
	for i in $(seq 2 "$N"); do
		objdump -d --no-show-raw-insn "$W/b$i" 2>/dev/null |
			awk '/<_?(mlsd_run|mlsd_lanes|mlsd_lanes_exec|mlsd_lanes_vec|decompress_multilane_avx512_mlsd)[a-z0-9_]*[.a-z0-9]*>:/{p=1; print; next} p&&/^$/{p=0} p' > "$W/lanes$i.s" || true
		echo "tree $i lane loop: $(grep -c -E '^\s*[0-9a-f]+:' "$W/lanes$i.s" || true) instructions, $(grep -c -E '\((%rsp|%rbp)\)|\[sp' "$W/lanes$i.s" || true) stack accesses, functions: $(grep -o -E '<_?mlsd_[a-z0-9_.]+>:' "$W/lanes$i.s" | tr '\n' ' ')" | tee -a "$RES"
	done
fi

FILES=()
for f in $NAMES; do
	FILES+=("$IN/$f")
done
SILFILES=()
for f in $SIL; do
	SILFILES+=("$IN/$f")
done
FZFILES=()
for f in $FUZZ_FILES; do
	FZFILES+=("$IN/$f")
done

ab() {
	local title=$1
	shift
	{
		echo
		echo "##### $title"
		"$PY" "$HERE/ab.py" -cpu "$CPU" "$@" | cut -c1-16,82-
	} | tee -a "$RES"
}

cd "$W"
ab "A/A control: base vs a copy of itself, level 6" \
	-r "$R2" "b0" "b0copy" -- -l 6 -n 5 -C "${FILES[@]}"
for i in $(seq 1 "$N"); do
	ab "base vs tree $i, level 6, whole files" \
		-r "$R" "b0" "b$i" -- -l 6 -n 5 -C "${FILES[@]}"
done
for i in $(seq 2 "$N"); do
	ab "base vs tree $i, level 1, whole files" \
		-r "$R2" "b0" "b$i" -- -l 1 -n 5 -C "${FILES[@]}"
	ab "base vs tree $i, level 12, Silesia" \
		-r "$R3" "b0" "b$i" -- -l 12 -n 5 -C "${SILFILES[@]}"
done
# Streams of a fixed size, each into an output buffer of exactly that size
for c in ${AB_CHUNKS:-262144 524288 1048576}; do
	for i in $(seq 2 "$N"); do
		ab "base vs tree $i, level 6, $c-byte streams" \
			-r "$R3" "b0" "b$i" -- -l 6 -c "$c" -n 5 -C "${FILES[@]}"
	done
done

# Differential fuzz: base decompressor vs each enabled tree's, with the
# multi-lane path forced on for every block.  The compressor comes from the
# base tree.
{
	echo
	echo "##### differential fuzz vs base (multi-lane forced on), $FUZZ_ITERS iterations x 6 streams per tree"
} | tee -a "$RES"
STOCK=()
for f in "$W/t0"/lib/*.c "$W/t0"/lib/*/*.c; do
	o="$W/stock_$(basename "$(dirname "$f")")_$(basename "$f" .c).o"
	$CC -O2 -std=gnu11 -I"$W/t0" -c "$f" -o "$o"
	STOCK+=("$o")
done
$CC -O2 -std=gnu11 -I"$W/t0" -include "$HERE/renameA.h" -c "$W/t0/lib/deflate_decompress.c" -o "$W/fzA.o"
for i in $(seq 2 "$N"); do
	$CC -O2 -std=gnu11 -I"$W/t$i" -DMLSD_MIN_PREV_BLOCK_OUT=0 -DMLSD_MIN_START_OUT=0 -include "$HERE/renameB.h" \
		-c "$W/t$i/lib/deflate_decompress.c" -o "$W/fzB$i.o"
	$CC -O2 -std=gnu11 -I"$W/t0" -o "$W/dfuzz$i" "$HERE/dfuzz.c" "$W/fzA.o" "$W/fzB$i.o" "${STOCK[@]}"
	for seed in 1 2; do
		echo "tree $i seed $seed: $("$W/dfuzz$i" "$FUZZ_ITERS" "$seed" "${FZFILES[@]}" 2>&1 | tail -3 | tr '\n' ' ')" | tee -a "$RES"
	done
	# Second pass with the tree's own gating off (multi-lane on every
	# eligible block, on any CPU vendor), if FUZZ_DEFS2 is set.
	if [ -n "${FUZZ_DEFS2:-}" ]; then
		$CC -O2 -std=gnu11 -I"$W/t$i" -DMLSD_MIN_PREV_BLOCK_OUT=0 -DMLSD_MIN_START_OUT=0 $FUZZ_DEFS2 -include "$HERE/renameB.h" \
			-c "$W/t$i/lib/deflate_decompress.c" -o "$W/fzB${i}x.o"
		$CC -O2 -std=gnu11 -I"$W/t0" -o "$W/dfuzz${i}x" "$HERE/dfuzz.c" "$W/fzA.o" "$W/fzB${i}x.o" "${STOCK[@]}"
		for seed in 3 4; do
			echo "tree $i ($FUZZ_DEFS2) seed $seed: $("$W/dfuzz${i}x" "$FUZZ_ITERS" "$seed" "${FZFILES[@]}" 2>&1 | tail -3 | tr '\n' ' ')" | tee -a "$RES"
		done
	fi
done
echo "ALL DONE" | tee -a "$RES"
