#!/bin/bash
# mlsd_ci run_ab.sh - test only: time master and the versions $VERS (NAME=REV,
# REV . for this tree; default old=$OLD new=.) on a runner, each at the code
# placements $PADS (a pad of that many bytes linked in front of
# deflate_decompress.o; builds NAME_pN).  Where the lanes are on as shipped at $OLD: levels 6, 1
# and 12.  Elsewhere: the lanes forced on, level 6.
# usage: run_ab.sh CORPUSDIR OUTDIR [BINDIR]
#        run_ab.sh -build BINDIR  (static binaries, shipped and forced, for
#                                  other runners: OUTDIR's BINDIR)
# With BINDIR, its builds get the prefix p_, and with NATIVE=1 the versions
# $NVERS (default $VERS) built by $CC on the runner (prefix n_) are timed with
# them.
set -euo pipefail
CC=${CC:-cc}; OLD=${OLD:-b2f1b4e}; PADS=${PADS:-0 16 32 48}
VERS=${VERS:-old=$OLD new=.}; NVERS=${NVERS:-$VERS}
HERE=$(cd "$(dirname "$0")" && pwd); TOP=$(cd "$HERE/../.." && pwd)
SRC=$(mktemp -d)
git -C "$TOP" archive aa5181e | (mkdir "$SRC/master" && tar -x -C "$SRC/master")
for v in $VERS $NVERS; do
	[ -e "$SRC/${v%%=*}" ] && continue
	if [ "${v#*=}" = . ]; then ln -s "$TOP" "$SRC/${v%%=*}"
	else git -C "$TOP" archive ${v#*=} | (mkdir "$SRC/${v%%=*}" && tar -x -C "$SRC/${v%%=*}"); fi
done
build() { # bindir name srcdir pad flags...
	local bin=$1 name=$2 src=$3 pad=$4 f srcs=; shift 4
	printf '\t.text\n\t.p2align 4\n\t.globl mlsd_ci_pad\nmlsd_ci_pad:\n\t.skip %d, 0xcc\n' $pad > "$SRC/pad_$name.s"
	for f in "$src"/lib/*.c; do
		[ "${f##*/}" = deflate_decompress.c ] && srcs="$srcs $SRC/pad_$name.s"
		srcs="$srcs $f"
	done
	$CC -O2 -DNDEBUG -std=gnu11 -I"$src" "$@" "$HERE/dbench.c" $srcs "$src"/lib/x86/*.c -o "$bin/dbench_$name"
	echo "$name: $(nm "$bin/dbench_$name" | grep -E ' (decompress_multilane_bmi2|deflate_decompress_bmi2)$' | tr '\n' ' ')" >> "$bin/loop.txt"
}
loopstat() { # bindir name srcdir flags...
	local bin=$1 name=$2 src=$3; shift 3
	$CC -O2 -DNDEBUG -std=gnu11 -I"$src" "$@" -c "$src/lib/deflate_decompress.c" -o "$SRC/dd_$name.o"
	objdump -d --no-show-raw-insn -M intel "$SRC/dd_$name.o" | awk '/<decompress_multilane_bmi2>:/{p=1} p&&/^$/{exit} p' > "$bin/ml_$name.s"
	echo "$name: $(python3 "$HERE/loop.py" "$bin/ml_$name.s" 2>&1 | head -1)" | tee -a "$bin/loop.txt"
}
builds() { # bindir prefix versions flags...
	local bin=$1 pre=$2 vers=$3 p v; shift 3
	for p in $PADS; do
		for v in $vers; do build "$bin" ${pre}${v%%=*}_p$p "$SRC/${v%%=*}" $p "$@"; done
	done
	for v in $vers; do loopstat "$bin" ${pre}${v%%=*} "$SRC/${v%%=*}" "$@"; done
}
if [ "${1:-}" = -build ]; then
	BIN=${2:?}; mkdir -p "$BIN"
	$CC --version | sed -n 1p > "$BIN/cc.txt"
	for p in $PADS; do build "$BIN" master_p$p "$SRC/master" $p -static; done
	builds "$BIN" "" "$VERS" -static
	builds "$BIN" F_ "$VERS" -static -DMLSD_TEST_ALWAYS_USABLE
	exit 0
fi
IN=${1:?}; OUT=${2:?}; PRE=${3:-}
mkdir -p "$OUT"/{bin,streams,tools,res,in}
cp "$HERE/abn.py" "$OUT/tools/"
{
	uname -a
	grep -m1 "model name" /proc/cpuinfo
	grep -E "^(vendor_id|cpu family|model|stepping)\s" /proc/cpuinfo | sort -u | head -4
	lscpu | grep -E "Model name|L1d|L2|CPU\(s\):" || true
	[ "${NATIVE:-}" != 1 ] || echo "native: $($CC --version | sed -n 1p)"
	if [ -n "$PRE" ]; then cat "$PRE/cc.txt"; else $CC --version | sed -n 1p; fi
} > "$OUT/res/system.txt" 2>&1
cat "$OUT/res/system.txt"
# allow_mlsd() at b2f1b4e
ON=$(awk -F': ' '/^vendor_id/{v=$2} /^cpu family/{f=$2} /^model\t/{m=$2} END {
	on = (v == "AuthenticAMD" && (f >= 26 || (f == 25 && ((m >= 16 && m < 32) || (m >= 96 && m < 176))))) ||
	     (v == "GenuineIntel" && f == 6 && (m == 173 || m == 207)); print on ? 1 : 0 }' /proc/cpuinfo)
echo "lanes on as shipped: $ON" | tee -a "$OUT/res/system.txt"
F=; [ $ON = 1 ] || F=F_
B=
if [ -n "$PRE" ]; then
	for p in $PADS; do
		cp "$PRE/dbench_master_p$p" "$OUT/bin/dbench_p_master_p$p"
		B=$B,p_master_p$p
		for v in $VERS; do
			cp "$PRE/dbench_$F${v%%=*}_p$p" "$OUT/bin/dbench_p_${v%%=*}_p$p"
			B=$B,p_${v%%=*}_p$p
		done
	done
	chmod +x "$OUT"/bin/dbench_*
	sed -n "s/^${F}\([a-z0-9]*\):/p_\1:/p" "$PRE/loop.txt" >> "$OUT/res/loop.txt"
fi
if [ -z "$PRE" ] || [ "${NATIVE:-}" = 1 ]; then
	n=; [ -z "$PRE" ] || n=n_
	for p in $PADS; do build "$OUT/bin" ${n}master_p$p "$SRC/master" $p; done
	vers=$VERS; [ -z "$PRE" ] || vers=$NVERS
	builds "$OUT/bin" "$n" "$vers" ${F:+-DMLSD_TEST_ALWAYS_USABLE}
	for p in $PADS; do B=$B,${n}master_p$p; for v in $vers; do B=$B,$n${v%%=*}_p$p; done; done
	cat "$OUT/bin/loop.txt" >> "$OUT/res/loop.txt"
fi
B=${B#,}
cc -O2 -I"$TOP" "$HERE/mkstream.c" "$TOP"/lib/*.c "$TOP"/lib/x86/*.c -o "$OUT/bin/mkstream"
SIL="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
LV="6 1 12"; [ $ON = 1 ] || LV=6
for f in $SIL enwik8; do
	ln -sf "$IN/$f" "$OUT/in/$f"
	for l in $LV; do "$OUT/bin/mkstream" $l 0 "$IN/$f" "$OUT/streams/$f.L$l.c0"; done
done
cd "$OUT"
export BENCH_CPU=${BENCH_CPU:-2}
for l in $LV; do
	files=in/*
	echo "== L$l $(uptime)" | tee -a res/bench.txt
	python3 tools/abn.py -o res/L$l.json -r ${ROUNDS:-3} -n 5 -ref ${B%%,*} -b $B -s .L$l.c0 $files | tee -a res/bench.txt
done
