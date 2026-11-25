#!/bin/bash
# nooutline_ci time.sh - test-only: timing of upstream master (HEAD~2) vs PR
# 487 (HEAD~1, -mno-outline on Apple), both built as CMake Release dylibs.
#
# Each library is timed alone in its own abp process ("solo"): loading two
# builds into one process makes them share branch-predictor and cache state,
# and on an M5 an identical copy of master next to master made the PR look 5%
# slower at decompression than it is.  So in every round, each file is timed
# by master, PR, and a copy of master's dylib (the A/A control) in three
# separate processes, back to back, in an order rotated by round and file;
# abp keeps the minimum over its reps, and the summary compares medians over
# rounds.  Before timing, one abp process with
# both builds checks that they give the same compressed output at every level
# and decompress each other's output.
#
# "aligned" repeats compression and decompression at level 6 with both trees
# built with -falign-functions=64, which keeps identical functions at the same
# alignment in both builds, to separate the effect of outlining from that of
# the code moving.
#
# usage: time.sh INDIR OUTDIR
#   XCODE=/Applications/Xcode_X.app selects the Xcode (default: the image's)
#   AB_COMP_LEVELS, AB_DECOMP_LEVELS, AB_ALIGNED (0 to skip), and rounds/reps:
#   AB_ROUNDS_FAST/AB_REPS_FAST (levels 1-9), AB_ROUNDS_SLOW/AB_REPS_SLOW
#   (10-12), AB_ROUNDS_D/AB_REPS_D (decompression)
# Writes OUTDIR/meta.txt (toolchain, CPU, outlining, sizes) and OUTDIR/res.txt
# (abp's lines under "=== TAG MODE LEVEL round R lib J" headers; lib 0 =
# master, 1 = PR, 2 = copy of master).  If the PR's arm64 code is identical to
# master's, nothing is timed.
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
COMP=${AB_COMP_LEVELS:-1 2 3 4 5 6 7 8 9 10 12}
DECOMP=${AB_DECOMP_LEVELS:-1 6}
META=$OUT/meta.txt
RES=$OUT/res.txt
: > "$RES"

tree base HEAD~2
tree pr HEAD~1
REL=(-DCMAKE_BUILD_TYPE=Release -DLIBDEFLATE_BUILD_STATIC_LIB=OFF -DLIBDEFLATE_BUILD_GZIP=OFF)
build "$W/src-base" "$W/b-base" "${REL[@]}"
build "$W/src-pr" "$W/b-pr" "${REL[@]}"
LB=$(dylib "$W/b-base")
LP=$(dylib "$W/b-pr")
mkdir -p "$W/copy"
LC=$W/copy/libdeflate-copy.dylib
cp "$LB" "$LC"
cc -O2 -Wall -o "$W/abp" "$HERE/abp.c"

{
	echo "image: ${IMAGE:-?}"
	echo "os: macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
	echo "xcode: $(xcodebuild -version | tr '\n' ' ' | sed 's/Build version/build/; s/ *$//') at $(xcode-select -p 2> /dev/null)${DEVELOPER_DIR:+ (DEVELOPER_DIR=$DEVELOPER_DIR)}"
	echo "cc: $(cc --version | head -1)"
	echo "cpu: $(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.ncpu) CPUs, $(( $(sysctl -n hw.memsize) >> 30 )) GiB"
	echo "master: $(git -C "$TOP" log -1 --format='%h %s' HEAD~2)"
	echo "pr: $(git -C "$TOP" log -1 --format='%h %s' HEAD~1)"
	echo "flag: HAVE_MNO_OUTLINE=$(cachevar "$W/b-pr" HAVE_MNO_OUTLINE), -mno-outline in $(flagcount "$W/b-pr") PR compile commands, $(flagcount "$W/b-base") master"
	echo "outlined: master $(outlined "$LB") pr $(outlined "$LP")"
	echo "text: master $(textsize "$LB") pr $(textsize "$LP")"
	echo "fastloop: master $(nm "$LB" | awk '/decompress_fastloop_unchecked/ { print $1 }') pr $(nm "$LP" | awk '/decompress_fastloop_unchecked/ { print $1 }')"
	echo "inputs: $(ls "$IN/real" | tr '\n' ' ')"
	head -3 "$IN/MANIFEST.txt" | sed 's/^/manifest: /'
} | tee "$META"
if same_text "$LB" "$LP"; then
	echo "identical: the PR's arm64 code is identical to master's; nothing to time" | tee -a "$META"
	exit 0
fi

# Same output at every level, and each build decompresses the other's.
for l in $COMP; do
	"$W/abp" -l "$l" -r 1 "$LB" "$LP" -- "$IN"/real/* > /dev/null
done
for l in $DECOMP; do
	"$W/abp" -d -l "$l" -r 1 "$LB" "$LP" -- "$IN"/real/* > /dev/null
	"$W/abp" -d -l "$l" -r 1 "$LP" "$LB" -- "$IN"/real/* > /dev/null
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
		solo main c "$l" "${AB_ROUNDS_SLOW:-5}" "${AB_REPS_SLOW:-2}" "$LB" "$LP" "$LC"
	else
		solo main c "$l" "${AB_ROUNDS_FAST:-8}" "${AB_REPS_FAST:-3}" "$LB" "$LP" "$LC"
	fi
done
for l in $DECOMP; do
	solo main d "$l" "${AB_ROUNDS_D:-15}" "${AB_REPS_D:-11}" "$LB" "$LP" "$LC"
done

if [ "${AB_ALIGNED:-1}" != 0 ]; then
	ALN=("${REL[@]}" -DCMAKE_C_FLAGS=-falign-functions=64)
	build "$W/src-base" "$W/a-base" "${ALN[@]}"
	build "$W/src-pr" "$W/a-pr" "${ALN[@]}"
	AB=$(dylib "$W/a-base")
	AP=$(dylib "$W/a-pr")
	AC=$W/copy/libdeflate-aligned-copy.dylib
	cp "$AB" "$AC"
	"$W/abp" -l 6 -r 1 "$AB" "$AP" "$LB" -- "$IN"/real/* > /dev/null
	echo "aligned: -falign-functions=64; outlined master $(outlined "$AB") pr $(outlined "$AP"); fastloop master $(nm "$AB" | awk '/decompress_fastloop_unchecked/ { print $1 }') pr $(nm "$AP" | awk '/decompress_fastloop_unchecked/ { print $1 }')" | tee -a "$META"
	solo aligned c 6 "${AB_ROUNDS_FAST:-8}" "${AB_REPS_FAST:-3}" "$AB" "$AP" "$AC"
	solo aligned d 6 "${AB_ROUNDS_D:-15}" "${AB_REPS_D:-11}" "$AB" "$AP" "$AC"
fi
echo "ALL DONE" | tee -a "$META"
