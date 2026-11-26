#!/bin/bash
# mlsd_ci diag.sh - test-only: run diag.c for trees 2..N of the series below
# HEAD (each built with -DMLSD_REPORT) on samba and dickens at level 1,
# reymont and samba at level 6, and reymont, mr and nci at level 12, in 3 separate
# processes per tree (to vary the memory layout).  Writes WORKDIR/diag.txt.  Not for upstream.
# usage: diag.sh INDIR WORKDIR
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
IN=${1:?usage: diag.sh INDIR WORKDIR}
OUT=${2:?usage: diag.sh INDIR WORKDIR}/diag.txt
W=$2/dg
N=${AB_SERIES:-3}
CC=${CC:-cc}
mkdir -p "$W"
: > "$OUT"
for i in 0 $(seq 2 "$N"); do
	rev="HEAD~$((N + 1 - i))"
	mkdir -p "$W/t$i"
	git archive "$rev" | tar -x -C "$W/t$i"
	echo "tree $i = $(git log -1 --format='%h %s' "$rev")" >> "$OUT"
done
STOCK=""
for f in "$W/t0"/lib/*.c "$W/t0"/lib/*/*.c; do
	o="$W/stock_$(basename "$(dirname "$f")")_$(basename "$f" .c).o"
	$CC -O2 -std=gnu11 -I"$W/t0" -c "$f" -o "$o"
	STOCK="$STOCK $o"
done
$CC -O2 -std=gnu11 -I"$W/t0" -include "$HERE/renameA.h" \
	-c "$W/t0/lib/deflate_decompress.c" -o "$W/A.o"
for i in $(seq 2 "$N"); do
	$CC -O2 -std=gnu11 -I"$W/t$i" -DMLSD_REPORT -DMLSD_REPORT_SYM=B_mlsd_report \
		-DMLSD_RACEHIST_SYM=B_mlsd_racehist \
		-include "$HERE/renameB.h" -c "$W/t$i/lib/deflate_decompress.c" -o "$W/B$i.o"
	$CC -O2 -std=gnu11 -I"$W/t0" -o "$W/diag$i" "$HERE/diag.c" "$W/A.o" "$W/B$i.o" $STOCK
done
for p in 1 2 3; do
	for i in $(seq 2 "$N"); do
		echo "## tree $i process $p" >> "$OUT"
		"$W/diag$i" 1 3 "$IN/samba" "$IN/dickens" >> "$OUT" 2>&1 || echo "diag tree $i L1 FAILED" >> "$OUT"
		"$W/diag$i" 6 3 "$IN/reymont" "$IN/samba" >> "$OUT" 2>&1 || echo "diag tree $i L6 FAILED" >> "$OUT"
		"$W/diag$i" 12 3 "$IN/reymont" "$IN/mr" "$IN/nci" >> "$OUT" 2>&1 || echo "diag tree $i L12 FAILED" >> "$OUT"
	done
done
echo "DIAG DONE" >> "$OUT"
