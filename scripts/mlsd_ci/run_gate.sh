#!/bin/bash
# run_gate.sh INDIR OUTDIR - test-only: per-block decode times with the lanes
# forced and with them off, plus each block's Huffman code statistics, for
# Silesia and enwik8 at levels 1, 6 and 12.  Writes OUTDIR/gate.csv.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
IN=${1:?}; W=${2:?}; CC=${CC:-clang}; REPS=${REPS:-5}
mkdir -p "$W/t0" "$W/t1"
git archive HEAD~2 | tar -x -C "$W/t0"
git archive HEAD~1 | tar -x -C "$W/t1"
STOCK=""
for f in "$W/t0"/lib/*.c "$W/t0"/lib/*/*.c; do
	o="$W/stock_$(basename "$(dirname "$f")")_$(basename "$f" .c).o"
	$CC -O2 -std=gnu11 -I"$W/t0" -c "$f" -o "$o"
	STOCK="$STOCK $o"
done
FORCED="-DMLSD_TEST_ANY_PRIORITY -DMLSD_MIN_PREV_OUT=0 -DMLSD_MIN_IN_BYTES=0 -DMLSD_MIN_OUT_BYTES=0 -DMLSD_MIN_LEN_SPACE=0"
for v in forced off; do
	fl=$FORCED; [ $v = off ] && fl=-DLIBDEFLATE_NO_MLSD
	$CC -O2 -DNDEBUG -fvisibility=hidden -DMLSD_DIAG $fl -include "$HERE/renameB.h" \
		-I"$W/t1" -c "$W/t1/lib/deflate_decompress.c" -o "$W/B_$v.o"
	$CC -O2 -I"$W/t0" -o "$W/diag2_$v" "$HERE/diag2.c" "$W/B_$v.o" $STOCK
done
{ uname -srm; sysctl -n machdep.cpu.brand_string 2>/dev/null || true; } > "$W/gate-system.txt"
: > "$W/gate.csv"
for l in 1 6 12; do
	for f in dickens mozilla mr nci ooffice osdb reymont samba sao webster xml x-ray enwik8; do
		[ -f "$IN/$f" ] || continue
		for v in off forced; do
			"$W/diag2_$v" $v $l "$REPS" "$IN/$f" >> "$W/gate.csv"
		done
	done
done
echo "rows: $(wc -l < "$W/gate.csv")" >> "$W/gate-system.txt"
