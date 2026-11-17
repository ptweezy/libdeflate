#!/bin/bash
# r8_time shard.sh - one timing shard (test-only): export the trees of the
# series, make the control trees, compile once, link this shard's layout
# seeds, time every job with lt.py, and leave everything agg.py needs in
# $R8_ROOT/res.
#
# Environment (the workflow sets these from plan.py's matrix):
#   R8_CFG      lb.py config (g2, c2, ...)
#   R8_SEEDS    this shard's seeds, e.g. 1001-1024 (unique per shard)
#   R8_JOBS     "LEVEL:CHUNK:ROUNDS:REPS[:CTL+CTL...] ..."
#   R8_FILESET  directory under R8_IN (real, adv, syn); default real
#   R8_NAME     shard name (default local)
#   R8_SERIES   commits in the series (default 2): m0 = HEAD~(N+1),
#               f1 = HEAD~N, ..., fN = HEAD~1
#   R8_TIMEOUT  the job's timeout in minutes (default 150); timing stops
#               starting rounds about 12 minutes before it
#   R8_LT_OPTS  extra lt.py options, e.g. "--order rot"
#   R8_IN       inputs.sh's output directory (default $RUNNER_TEMP/r8in)
#   R8_ROOT     work directory (default $RUNNER_TEMP/r8t)
#   R8_CI       1 on a hosted runner: allows the two sudo steps (perf
#               paranoia, moving other processes off the timing core).
#               Never set it on a machine you share.
#   R8_SKIP_TREES  1: use the trees already in $R8_ROOT/src (local tests)
#   R8_CORE, R8_DRIVER_CPUS  override the timing CPU / the CPUs for the rest
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${R8_ROOT:-${RUNNER_TEMP:-/tmp}/r8t}
IN=${R8_IN:-${RUNNER_TEMP:-/tmp}/r8in}
CFG=${R8_CFG:?}
SEEDS=${R8_SEEDS:?}
JOBS=${R8_JOBS:?}
FILESET=${R8_FILESET:-real}
NAME=${R8_NAME:-local}
SERIES=${R8_SERIES:-2}
MODE=${R8_MODE:-anchor}
TIMEOUT=${R8_TIMEOUT:-150}
LTOPTS=${R8_LT_OPTS:-}
START=$(date +%s)
RES=$ROOT/res
SRC=$ROOT/src
mkdir -p "$RES" "$SRC"
export R8LAY_ROOT=$ROOT/lay R8LAY_SRC=$SRC R8LAY_LDBENCH=$HERE/ldbench.c

# --- trees -------------------------------------------------------------
TREES=m0
for i in $(seq 1 "$SERIES"); do TREES="$TREES f$i"; done
BASE_SHA=local
HEAD_SHA=local
if [ -z "${R8_SKIP_TREES:-}" ]; then
	TOP=$(git -C "$HERE" rev-parse --show-toplevel)
	: > "$RES/trees.txt"
	i=0
	for n in $TREES; do
		c=$(git -C "$TOP" rev-parse "HEAD~$((SERIES + 1 - i))")
		rm -rf "$SRC/$n-src"; mkdir -p "$SRC/$n-src"
		git -C "$TOP" archive "$c" | tar -x -C "$SRC/$n-src"
		echo "$n $(git -C "$TOP" log --format='%H %s' -1 "$c")" | tee -a "$RES/trees.txt"
		i=$((i + 1))
	done
	BASE_SHA=$(git -C "$TOP" rev-parse "HEAD~$((SERIES + 1))")
	HEAD_SHA=$(git -C "$TOP" rev-parse HEAD)
fi
CTL=$(for j in $JOBS; do echo "$j" | cut -s -d: -f5 | tr '+' '\n'; done | sort -u | tr '\n' ' ')
if [ -n "${CTL// /}" ]; then
	# shellcheck disable=SC2086
	python3 "$HERE/mkctl.py" "$SRC" m0 $CTL
fi
ALL="$TREES $CTL"

# --- build -------------------------------------------------------------
# shellcheck disable=SC2086
{
python3 "$HERE/lb.py" obj "$CFG" $ALL
FS=OK; python3 "$HERE/lb.py" fscheck "$CFG" $ALL > "$RES/fscheck.txt" || FS=FAILED
tail -n 12 "$RES/fscheck.txt"
python3 "$HERE/lb.py" link "$CFG" plain plain $TREES
python3 "$HERE/lb.py" link "$CFG" "$MODE" "$SEEDS" $ALL
S0=${SEEDS%%[-,]*}
case "$SEEDS" in
*,*) CKS=$SEEDS ;;
*-*) S9=${SEEDS#*-}; CKS=$S0-$(( S0 + 2 < S9 ? S0 + 2 : S9 )) ;;
*) CKS=$SEEDS ;;
esac
CK=OK; python3 "$HERE/lb.py" check "$CFG" "$MODE" "$CKS" $ALL > "$RES/check.txt" || CK=FAILED
cat "$RES/check.txt"
}
B1=$R8LAY_ROOT-b/$CFG.$MODE/m0/$(printf 's%03d' "$S0")
cp "$B1/lay.ld" "$RES/sample-lay.ld"
cp "$B1/map.json" "$RES/sample-map.json"

# --- machine -----------------------------------------------------------
read -r CORE DRV < <(python3 "$HERE/sysinfo.py" cpus)
CORE=${R8_CORE:-$CORE}
DRV=${R8_DRIVER_CPUS:-$DRV}
SHIELD=no
if [ "${R8_CI:-}" = 1 ] && sudo -n true 2>/dev/null; then
	# user-mode cycle counters, if the VM has a PMU at all (ldbench -T tells)
	sudo -n sysctl -w kernel.perf_event_paranoid=1 >/dev/null 2>&1 || true
	# best effort: every existing thread off the timing CPU's core; the timed
	# processes are put on $CORE explicitly by lt.py
	sudo -n bash -c "for p in /proc/[0-9]*; do taskset -a -cp $DRV \${p#/proc/} >/dev/null 2>&1; done; true"
	SHIELD=yes
fi
echo "timing CPU $CORE, everything else on $DRV (shield: $SHIELD)"
TIMER=$(taskset -c "$CORE" "$B1/ldbench" -T)
echo "$TIMER"
FRESH=/dev/shm
if ! (cp "$B1/ldbench" /dev/shm/r8lay_probe_$$ && /dev/shm/r8lay_probe_$$ -T >/dev/null); then
	FRESH=$ROOT/fresh; mkdir -p "$FRESH"
fi
rm -f /dev/shm/r8lay_probe_$$
MAN=$(grep -E '^(corpus|real)' "$IN/MANIFEST.txt" 2>/dev/null | tr '\n' ';' || true)
python3 "$HERE/sysinfo.py" meta "$RES/meta.json" "name=$NAME" "cfg=$CFG" "seeds=$SEEDS" "jobs=$JOBS" \
	"fileset=$FILESET" "mode=$MODE" "series=$SERIES" "trees=$ALL" "base_sha=$BASE_SHA" \
	"head_sha=$HEAD_SHA" "timing_cpu=$CORE" "driver_cpus=$DRV" "shield=$SHIELD" "timer=$TIMER" \
	"fscheck=$FS" "check=$CK" "fresh_dir=$FRESH" "inputs=$MAN" "ltopts=$LTOPTS"

# --- time --------------------------------------------------------------
DEADLINE=$((START + TIMEOUT * 60 - 720))
NJ=$(echo $JOBS | wc -w)
RC=0
k=0
for j in $JOBS; do
	IFS=: read -r L C N R X <<< "$j"
	T=$(echo $TREES ${X//+/ } | tr ' ' ',')
	o=$RES/${NAME}__L${L}_s${C}
	now=$(date +%s)
	budget=$(( (DEADLINE - now) / (NJ - k) ))
	if [ "$budget" -lt 60 ]; then
		echo "SKIPPED $j: out of time" | tee -a "$RES/skipped.txt"
		k=$((k + 1)); continue
	fi
	rc=0
	python3 "$HERE/lt.py" -o "$o.json" -b "$CFG.$MODE" -t "$T" --aa --seeds "$SEEDS" \
		-l "$L" -s "$C" -n "$N" -r "$R" --core "$CORE" --driver-cpus "$DRV" \
		--files "dir:$IN/$FILESET" --budget "$budget" --fresh-dir "$FRESH" $LTOPTS || rc=$?
	[ $rc = 0 ] || { echo "lt.py exit $rc for $j" | tee -a "$RES/errors.txt"; RC=1; }
	if [ -f "$o.json" ]; then
		python3 "$HERE/la.py" -f "$o.json" | tee "$o.txt"
	fi
	k=$((k + 1))
done
echo "shard $NAME done in $(( $(date +%s) - START )) s (exit $RC)"
exit $RC
