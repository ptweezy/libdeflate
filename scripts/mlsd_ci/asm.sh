#!/bin/bash
# mlsd_ci asm.sh - test only: compile lib/deflate_decompress.c of this tree
# ("base") and of each variant NAME (scripts/mlsd_ci/variants/NAME.patch
# applied, and the compiler flags in NAME.flags) with every gcc and
# clang on the runner (the flags of run_bench.sh), disassemble the lanes' and
# the fastloop's functions, and print the step loop's stats to OUT/loop.txt.
# usage: asm.sh OUTDIR
set -uo pipefail
OUT=${1:?}; HERE=$(cd "$(dirname "$0")" && pwd); TOP=$(cd "$HERE/../.." && pwd)
mkdir -p "$OUT/out" "$OUT/src"
CCS=
for c in gcc-9 gcc-10 gcc-11 gcc-12 gcc-13 gcc-14 clang-13 clang-14 clang-15 clang-16 clang-17 clang-18 clang-19 clang-20 clang-21 gcc-15; do
	command -v $c > /dev/null && CCS="$CCS $c"
done
{ uname -a; grep -m1 "model name" /proc/cpuinfo; for c in $CCS; do $c --version | sed -n 1p; done; } > "$OUT/out/system.txt" 2>&1
cat "$OUT/out/system.txt"
FNS="decompress_multilane_bmi2 decompress_fastloop_unchecked_bmi2 decompress_fastloop_checked_bmi2"
for v in base $(ls "$HERE/variants" 2> /dev/null | sed -n 's/\.\(patch\|flags\)$//p' | sort -u); do
	VF=$(cat "$HERE/variants/$v.flags" 2> /dev/null || true)
	src="$OUT/src/$v"; mkdir -p "$src"
	git -C "$TOP" archive HEAD lib common_defs.h libdeflate.h | tar -x -C "$src"
	if [ -e "$HERE/variants/$v.patch" ] && ! patch -s -d "$src" -p1 < "$HERE/variants/$v.patch"; then
		echo "$v: patch failed" | tee -a "$OUT/out/loop.txt"; continue
	fi
	for c in $CCS; do
		o="$OUT/out/$v/$c"; mkdir -p "$o"
		if ! $c -O2 -DNDEBUG -std=gnu11 $VF -I"$src" -c "$src/lib/deflate_decompress.c" -o "$o/dd.o" 2> "$o/cc.err"; then
			echo "$v $c: build failed" | tee -a "$OUT/out/loop.txt"; cat "$o/cc.err"; continue
		fi
		$c -O2 -DNDEBUG -std=gnu11 $VF -I"$src" -S -masm=intel "$src/lib/deflate_decompress.c" -o - 2> /dev/null | grep -v '^\s*\.cfi_' > "$o/dd.S"
		case $c in clang*) $c -O2 -DNDEBUG -std=gnu11 $VF -I"$src" -c "$src/lib/deflate_decompress.c" -o /dev/null -Rpass-missed=regalloc 2> "$o/regalloc.txt" ;; esac
		objdump -d --no-show-raw-insn -M intel "$o/dd.o" > "$o/dd.dis"
		for fn in $FNS; do
			awk "/<$fn(\\.[a-z]+\\.[0-9]+)*>:/{p=1} p&&/^\$/{exit} p" "$o/dd.dis" > "$o/$fn.s"
		done
		echo "$v $c: $(python3 "$HERE/loop.py" "$o/decompress_multilane_bmi2.s" 2>&1 | head -1), size $(grep -c . "$o/decompress_multilane_bmi2.s") / fastloop $(grep -c . "$o/decompress_fastloop_unchecked_bmi2.s")" | tee -a "$OUT/out/loop.txt"
		rm -f "$o/dd.o"
	done
done
