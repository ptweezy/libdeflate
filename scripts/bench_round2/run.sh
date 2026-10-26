#!/bin/bash
# Benchmark the round-2 commits on a CI runner (test-only; not for upstream).
#
# The branch is master + 5 commits, so the cumulative states are:
#   c0 = master, c1 = +near-optimal pass skip, c2 = +build_decode_table,
#   c3 = +fastloop to end, c4 = +sort_symbols, c5 = +precode items.
# Comparisons: L12/L11 compression c0 vs c1; decompression c1 vs c2 vs c3;
# small-input compression c3 vs c4 vs c5.  Each variant is built with
# -falign-functions=4096 so unchanged hot functions keep their addresses.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(git -C "$HERE" rev-parse --show-toplevel)
W=${RUNNER_TEMP:-/tmp}/bench_round2
mkdir -p "$W/corpus/silesia"
CC=${CC:-cc}

get_corpus() {
	cd "$W/corpus"
	if curl -fsSL --retry 3 --max-time 300 -o silesia.zip \
		https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip &&
	   unzip -q -o silesia.zip -d silesia; then
		echo "corpus: Silesia"
		return
	fi
	echo "corpus: Silesia download failed; using files from this runner"
	rm -rf silesia; mkdir silesia
	find /usr/include /usr/share/doc -type f -size -2M 2>/dev/null | head -20000 |
		xargs cat 2>/dev/null | head -c 30000000 > silesia/text || true
	find /usr/lib /usr/bin -type f -size +100k 2>/dev/null | head -400 |
		xargs cat 2>/dev/null | head -c 30000000 > silesia/binary || true
	tar -cf - -C "$TOP" lib programs 2>/dev/null | head -c 10000000 > silesia/srctar || true
}
get_corpus
ls -l "$W/corpus/silesia"

B=$(git -C "$TOP" rev-parse HEAD~1)   # HEAD is the commit that adds this script
for i in 0 1 2 3 4 5; do
	c=$(git -C "$TOP" rev-parse "$B~$((5 - i))")
	rm -rf "$W/c$i-src" "$W/c$i"; mkdir -p "$W/c$i-src" "$W/c$i"
	git -C "$TOP" archive "$c" | tar -x -C "$W/c$i-src"
	$CC -O2 -falign-functions=4096 -std=c11 -w -I"$W/c$i-src" -o "$W/c$i/ldbench" \
		"$HERE/ldbench.c" "$W/c$i-src"/lib/*.c "$W/c$i-src"/lib/x86/*.c \
		"$W/c$i-src"/lib/arm/*.c
	echo "built c$i = $(git -C "$TOP" log --oneline -1 "$c")"
done

export LDDIR=$W LDCORPUS=$W/corpus
A="python3 $HERE/abn.py --core 1 --files all"
echo "=== $(uname -sm); $($CC --version | head -1)"
grep -m1 "model name" /proc/cpuinfo 2>/dev/null || sysctl -n machdep.cpu.brand_string 2>/dev/null || true
echo "=== compression L12, L11: c0 (master) vs c1 (pass skip)"
$A -m c -n 5 -r 1 -l 12,11 c0 c1
for s in 1024 4096 0; do
	echo "=== decompression chunk=$s: c1 vs c2 (build_decode_table) vs c3 (+fastloop to end)"
	$A -m d -n 5 -r 9 -l 6 -s $s c1 c2 c3
done
for s in 1024 4096 0; do
	echo "=== compression chunk=$s, L1/L6: c3 vs c4 (sort_symbols) vs c5 (+precode items)"
	$A -m c -n 5 -r 3 -l 1,6 -s $s c3 c4 c5
done
