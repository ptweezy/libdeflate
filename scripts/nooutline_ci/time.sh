#!/bin/bash
# nooutline_ci time.sh - test-only: timing of a base tree against a test tree
# (and optionally an extra tree), all built as CMake Release shared libraries.
#
# Each library is timed alone in its own abp process ("solo"): loading two
# builds into one process makes them share branch-predictor and cache state,
# and on an M5 an identical copy of master next to master made the PR look 5%
# slower at decompression than it is.  So in every round, each file is timed
# by every library (base, test, a copy of base's library as the A/A control,
# and the extra tree's library if any) in separate processes, back to back,
# in an order rotated by round and file; abp keeps the minimum over its reps,
# and the summary compares medians over rounds.  Before timing, one abp
# process with all builds checks that they give the same compressed output at
# every level and decompress each other's output.
#
# "aligned" repeats compression and decompression at level 6 with base and
# test built with -falign-functions=64, which keeps identical functions at the
# same alignment in both builds, to separate the effect of a change from that
# of code moving.
#
# usage: time.sh INDIR OUTDIR
#   AB_BASE (default HEAD~2), AB_TEST (default HEAD~1), AB_EXTRA (default
#   none): the trees, as git revisions
#   XCODE=/Applications/Xcode_X.app selects the Xcode (macOS; default: the
#   selected one)
#   AB_COMP_LEVELS, AB_DECOMP_LEVELS, AB_ALIGNED (0 to skip), and rounds/reps:
#   AB_ROUNDS_FAST/AB_REPS_FAST (levels 1-9), AB_ROUNDS_SLOW/AB_REPS_SLOW
#   (10-12), AB_ROUNDS_D/AB_REPS_D (decompression)
# Writes OUTDIR/meta.txt (toolchain, CPU, outlining, sizes, fastloop
# placement) and OUTDIR/res.txt (abp's lines under "=== TAG MODE LEVEL round R
# lib J" headers; lib 0 = base, 1 = test, 2 = copy of base, 3 = extra).  If
# the test tree's code is identical to the base tree's, nothing is timed.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
IN=${1:?usage: time.sh INDIR OUTDIR}
OUT=${2:?usage: time.sh INDIR OUTDIR}
W=$OUT/w
mkdir -p "$W"
. "$HERE/lib.sh"
if [ -n "${XCODE:-}" ]; then
	export DEVELOPER_DIR=$XCODE/Contents/Developer
fi
BASE_REV=${AB_BASE:-HEAD~2}
TEST_REV=${AB_TEST:-HEAD~1}
EXTRA_REV=${AB_EXTRA:-}
COMP=${AB_COMP_LEVELS:-1 2 3 4 5 6 7 8 9 10 12}
DECOMP=${AB_DECOMP_LEVELS:-1 6}
META=$OUT/meta.txt
RES=$OUT/res.txt
: > "$RES"

tree base "$BASE_REV"
tree test "$TEST_REV"
REL=(-DCMAKE_BUILD_TYPE=Release -DLIBDEFLATE_BUILD_STATIC_LIB=OFF -DLIBDEFLATE_BUILD_GZIP=OFF)
build "$W/src-base" "$W/b-base" "${REL[@]}"
build "$W/src-test" "$W/b-test" "${REL[@]}"
LB=$(dylib "$W/b-base")
LT=$(dylib "$W/b-test")
mkdir -p "$W/copy"
LC=$W/copy/copy-$(basename "$LB")
cp "$LB" "$LC"
LIBS=("$LB" "$LT" "$LC")
if [ -n "$EXTRA_REV" ]; then
	tree extra "$EXTRA_REV"
	build "$W/src-extra" "$W/b-extra" "${REL[@]}"
	LX=$(dylib "$W/b-extra")
	LIBS+=("$LX")
fi
cc -O2 -Wall -o "$W/abp" "$HERE/abp.c" -ldl 2> /dev/null || cc -O2 -Wall -o "$W/abp" "$HERE/abp.c"

each() {  # each FUNC: "base X test Y [extra Z]"
	local s="base $($1 "$LB") test $($1 "$LT")"
	[ -z "$EXTRA_REV" ] || s="$s extra $($1 "$LX")"
	echo "$s"
}
{
	echo "image: ${IMAGE:-?}"
	if is_macho; then
		echo "os: macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
		echo "xcode: $(xcodebuild -version | tr '\n' ' ' | sed 's/Build version/build/; s/ *$//') at $(xcode-select -p 2> /dev/null)${DEVELOPER_DIR:+ (DEVELOPER_DIR=$DEVELOPER_DIR)}"
		echo "cpu: $(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.ncpu) CPUs, $(( $(sysctl -n hw.memsize) >> 30 )) GiB"
	else
		echo "os: $(uname -sr), $(. /etc/os-release 2> /dev/null && echo "$PRETTY_NAME")"
		echo "cpu: $(lscpu | sed -n 's/^Model name: *//p' | head -1) ($(uname -m)), $(ncpu) CPUs, $(( $(awk '/MemTotal/ { print $2 }' /proc/meminfo) >> 20 )) GiB"
	fi
	echo "cc: $(cc --version | head -1)"
	echo "base: $(git -C "$TOP" log -1 --format='%h %s' "$BASE_REV")"
	echo "test: $(git -C "$TOP" log -1 --format='%h %s' "$TEST_REV")"
	[ -z "$EXTRA_REV" ] || echo "extra: $(git -C "$TOP" log -1 --format='%h %s' "$EXTRA_REV")"
	echo "flag: HAVE_MNO_OUTLINE=$(cachevar "$W/b-test" HAVE_MNO_OUTLINE), -mno-outline in $(flagcount "$W/b-test") test compile commands, $(flagcount "$W/b-base") base"
	echo "outlined: $(each outlined)"
	echo "text: $(each textsize)"
	echo "fastloop: $(each fastloops)"
	echo "inputs: $(ls "$IN/real" | tr '\n' ' ')"
	head -3 "$IN/MANIFEST.txt" | sed 's/^/manifest: /'
} | tee "$META"
if same_text "$LB" "$LT"; then
	echo "identical: the test tree's code is identical to the base tree's; nothing to time" | tee -a "$META"
	exit 0
fi

# Same output at every level, and each build decompresses the others'.
for l in $COMP; do
	"$W/abp" -l "$l" -r 1 "${LIBS[@]}" -- "$IN"/real/* > /dev/null
done
for l in $DECOMP; do
	"$W/abp" -d -l "$l" -r 1 "$LB" "$LT" ${LX:+"$LX"} -- "$IN"/real/* > /dev/null
	"$W/abp" -d -l "$l" -r 1 "$LT" "$LB" -- "$IN"/real/* > /dev/null
done
echo "identity: same compressed output at levels $(echo $COMP | tr ' ' ','); decompression cross-checked at levels $(echo $DECOMP | tr ' ' ',')" | tee -a "$META"

# solo TAG MODE LEVEL ROUNDS REPS LIB...: ROUNDS rounds; in each, for each
# file, every LIB times that file in its own abp process, back to back, in an
# order rotated by round and file, so that a slow spell of the machine hits
# all LIBs on the same file rather than one LIB on many files.
solo() {
	local tag=$1 mode=$2 level=$3 rounds=$4 reps=$5 d= r i k j t0
	shift 5
	local libs=("$@")
	local n=${#libs[@]}
	local files=("$IN"/real/*)
	[ "$mode" = d ] && d=-d
	t0=$SECONDS
	for ((r = 0; r < rounds; r++)); do
		for ((i = 0; i < ${#files[@]}; i++)); do
			for ((k = 0; k < n; k++)); do
				j=$(((k + r + i) % n))
				echo "=== $tag $mode $level round $r lib $j" >> "$RES"
				"$W/abp" $d -l "$level" -r "$reps" "${libs[$j]}" -- "${files[$i]}" >> "$RES"
			done
		done
	done
	printf '%-8s %s L%-2s %3ds  ' "$tag" "$mode" "$level" $((SECONDS - t0))
	python3 "$HERE/summarize.py" --one "$RES" "$tag" "$mode" "$level"
}

for l in $COMP; do
	if [ "$l" -ge 10 ]; then
		solo main c "$l" "${AB_ROUNDS_SLOW:-5}" "${AB_REPS_SLOW:-2}" "${LIBS[@]}"
	else
		solo main c "$l" "${AB_ROUNDS_FAST:-8}" "${AB_REPS_FAST:-3}" "${LIBS[@]}"
	fi
done
for l in $DECOMP; do
	solo main d "$l" "${AB_ROUNDS_D:-15}" "${AB_REPS_D:-11}" "${LIBS[@]}"
done

if [ "${AB_ALIGNED:-1}" != 0 ]; then
	ALN=("${REL[@]}" -DCMAKE_C_FLAGS=-falign-functions=64)
	build "$W/src-base" "$W/a-base" "${ALN[@]}"
	build "$W/src-test" "$W/a-test" "${ALN[@]}"
	AB=$(dylib "$W/a-base")
	AT=$(dylib "$W/a-test")
	AC=$W/copy/aligned-copy-$(basename "$AB")
	cp "$AB" "$AC"
	"$W/abp" -l 6 -r 1 "$AB" "$AT" "$LB" -- "$IN"/real/* > /dev/null
	echo "aligned: -falign-functions=64; outlined base $(outlined "$AB") test $(outlined "$AT"); fastloop base $(fastloops "$AB") test $(fastloops "$AT")" | tee -a "$META"
	solo aligned c 6 "${AB_ROUNDS_FAST:-8}" "${AB_REPS_FAST:-3}" "$AB" "$AT" "$AC"
	solo aligned d 6 "${AB_ROUNDS_D:-15}" "${AB_REPS_D:-11}" "$AB" "$AT" "$AC"
fi
echo "ALL DONE" | tee -a "$META"
