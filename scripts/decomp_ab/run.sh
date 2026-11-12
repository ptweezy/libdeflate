#!/bin/bash
# decomp_ab run.sh - test-only: interleaved A/B timing of decompression for the
# commit series below HEAD.  Not for upstream.
# usage: run.sh INDIR WORKDIR
#
# The trees are tree 0 = HEAD~(N+1) (the base), tree 1 = HEAD~N, ..., tree N =
# HEAD~1; HEAD is the test-only commit.  N is $AB_SERIES (default 2).  Each tree
# is built into the bench harness with $CC (default cc), unless $AB_PREBUILT is
# set, in which case WORKDIR/b0$EXE .. bN$EXE must exist already.
#
# Every comparison is base vs one tree, on all files of INDIR, interleaved per
# file by ab.py.  Times are wall-clock (hosted runners have no cycle counter);
# "d min%" compares the best time of each side and "d med%" is the median of
# the paired per-round ratios.  Decompressed output is verified on every run.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
IN=${1:?usage: run.sh INDIR WORKDIR}
W=${2:?usage: run.sh INDIR WORKDIR}
N=${AB_SERIES:-2}
CC=${CC:-cc}
CPU=${AB_CPU:-2}
EXE=${EXE:-}
PY=${PY:-python3}
R=${AB_ROUNDS:-7}
R2=${AB_ROUNDS2:-5}
R3=${AB_ROUNDS3:-3}
NAMES=${AB_FILES:-dickens mozilla mr nci ooffice osdb reymont samba sao webster xml x-ray enwik8}
mkdir -p "$W"
RES=$W/results.txt
: > "$RES"

{
	echo "== system"
	uname -srm
	lscpu 2>/dev/null | grep -E 'Model name|^CPU\(s\)|Thread|L1d|L2 |L3 ' ||
		sysctl -n machdep.cpu.brand_string 2>/dev/null ||
		powershell -NoProfile -Command "(Get-CimInstance Win32_Processor).Name" 2>/dev/null ||
		true
	[ -n "${AB_PREBUILT:-}" ] || $CC --version | head -1
} | tee -a "$RES"

for i in $(seq 0 "$N"); do
	rev="HEAD~$((N + 1 - i))"
	if [ -z "${AB_PREBUILT:-}" ]; then
		mkdir -p "$W/t$i"
		git -C "$TOP" archive "$rev" | tar -x -C "$W/t$i"
		$CC -O2 -std=gnu11 -I"$W/t$i" -o "$W/b$i" "$HERE/bench.c" \
			"$W/t$i"/lib/*.c "$W/t$i"/lib/*/*.c
	fi
	echo "tree $i = $(git -C "$TOP" log -1 --format='%h %s' "$rev")" | tee -a "$RES"
done
cp "$W/b0$EXE" "$W/b0copy$EXE"

# Diagnostic only: how the last tree's branchless fastloop was compiled.
if command -v objdump > /dev/null 2>&1; then
	objdump -d "$W/b$N$EXE" 2>/dev/null |
		awk '/branchless_fastloop>:/{p=1; print; next} p&&/^$/{p=0} p' > "$W/bl.s" || true
	echo "branchless fastloop: $(grep -c -E 'csel|csinc|cmov' "$W/bl.s" || true) conditional selects in $(wc -l < "$W/bl.s") lines" | tee -a "$RES"
	grep -n -E 'csel|csinc|cmov' "$W/bl.s" | head -12 | tee -a "$RES" || true
fi

FILES=""
for f in $NAMES; do
	FILES="$FILES $IN/$f"
done

ab() {
	local title=$1
	shift
	{
		echo
		echo "##### $title"
		"$PY" "$HERE/ab.py" -cpu "$CPU" "$@" $FILES | cut -c1-16,82-
	} | tee -a "$RES"
}

cd "$W"
ab "A/A control: base vs a copy of itself, level 6, whole files" \
	-r "$R2" "b0$EXE" "b0copy$EXE" -- -l 6 -n 5 -C
for i in $(seq 1 "$N"); do
	ab "base vs tree $i, level 6, whole files" \
		-r "$R" "b0$EXE" "b$i$EXE" -- -l 6 -n 5 -C
done
ab "base vs tree $N, level 1, whole files" \
	-r "$R2" "b0$EXE" "b$N$EXE" -- -l 1 -n 5 -C
ab "base vs tree $N, level 6, 64 KiB chunks" \
	-r "$R3" "b0$EXE" "b$N$EXE" -- -l 6 -n 3 -C -c 65536
ab "base vs tree $N, level 6, 4 KiB chunks" \
	-r "$R3" "b0$EXE" "b$N$EXE" -- -l 6 -n 3 -C -c 4096
echo "ALL DONE" | tee -a "$RES"
