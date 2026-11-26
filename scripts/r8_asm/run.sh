#!/bin/bash
# Test-only: compile lib/deflate_compress.c of several trees with the
# runner's gcc and clang (the r8-time flags) and save the disassembly,
# function sizes and inlining remarks.  usage: run.sh OUTDIR NAME=REV...
set -eu
out=$1; shift
mkdir -p "$out"
gcc --version | head -1 > "$out/versions.txt"
clang --version | head -1 >> "$out/versions.txt"
for spec in "$@"; do
	name=${spec%%=*}; rev=${spec#*=}
	src=$RUNNER_TEMP/src-$name
	mkdir -p "$src"
	git archive "$rev" | tar -x -C "$src"
	for cc in gcc clang; do
		fl="-O2 -std=c11 -D_GNU_SOURCE -Wall -fvisibility=hidden -I$src -ffunction-sections"
		o=$out/$cc-$name
		$cc $fl -g -c "$src/lib/deflate_compress.c" -o "$o.o"
		objdump -d --no-show-raw-insn "$o.o" > "$o.dis"
		nm -S --size-sort "$o.o" > "$o.nm"
		if [ $cc = clang ]; then
			$cc $fl -c "$src/lib/deflate_compress.c" -o /dev/null \
				-Rpass=inline -Rpass-missed=inline 2> "$o.inline.txt" || true
		else
			$cc $fl -c "$src/lib/deflate_compress.c" -o /dev/null \
				-fopt-info-inline-optimized-missed="$o.inline.txt" || true
		fi
		$cc $fl -fstack-usage -c "$src/lib/deflate_compress.c" -o /dev/null -dumpbase "$o" -dumpdir "$out/" 2>/dev/null || \
			(cd "$out" && $cc $fl -fstack-usage -c "$src/lib/deflate_compress.c" -o "$cc-$name.su.o" && rm -f "$cc-$name.su.o") || true
	done
done
ls -la "$out"
