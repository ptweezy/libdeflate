#!/bin/bash
# nooutline_ci sweep.sh - test-only (Apple arm64): where in a 128-byte cache
# line should the decompression fastloop sit?
#
# Builds master (AB_BASE, default HEAD~2) and the PR (AB_TEST, default HEAD~1)
# as CMake Release dylibs, then 8 variants of the PR's dylib in which
# decompress_fastloop_unchecked_neon() starts K = 0, 16, ..., 112 bytes past a
# 128-byte boundary and the code after it is realigned to 128 bytes, so that
# only the fastloop's placement changes: deflate_decompress.c is compiled to
# assembly with CMake's exact command, padded there (padasm.py), reassembled,
# and the library is relinked with CMake's own link command.  A control built
# the same way without padding must be byte-identical to CMake's dylib, and
# every variant's fastloop must have the PR's instructions.
#
# Then times decompression of master, the PR, a copy of master (A/A) and the
# 8 variants, each library alone in its own abp process per file, rounds in
# an order rotated by round and file (as in time.sh).
#
# usage: sweep.sh INDIR OUTDIR
#   AB_DECOMP_LEVELS (default "6 12"), AB_ROUNDS_D (8), AB_REPS_D (11)
#   XCODE=/Applications/Xcode_X.app selects the Xcode
# Writes OUTDIR/meta.txt and OUTDIR/res.txt (lib numbers as in meta's libs:
# line), and prints sweep_summary.py's table at the end.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
IN=$(cd "${1:?usage: sweep.sh INDIR OUTDIR}" && pwd)
OUT=${2:?usage: sweep.sh INDIR OUTDIR}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
W=$OUT/w
mkdir -p "$W"
. "$HERE/lib.sh"
if [ -n "${XCODE:-}" ]; then
	export DEVELOPER_DIR=$XCODE/Contents/Developer
fi
BASE_REV=${AB_BASE:-HEAD~2}
TEST_REV=${AB_TEST:-HEAD~1}
DECOMP=${AB_DECOMP_LEVELS:-6 12}
FUNC=_decompress_fastloop_unchecked_neon
KS="0 16 32 48 64 80 96 112"
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
cc -O2 -Wall -o "$W/abp" "$HERE/abp.c"

# CMake's compile command for deflate_decompress.c, turned into one that writes
# assembly.
OBJREL=CMakeFiles/libdeflate_shared.dir/lib/deflate_decompress.c.o
python3 - "$W/b-test" "$W/to_s.sh" <<'EOF'
import json, shlex, sys
b, script = sys.argv[1], sys.argv[2]
for e in json.load(open(b + '/compile_commands.json')):
    if e['file'].endswith('lib/deflate_decompress.c'):
        a = shlex.split(e['command'])
        i = a.index('-o'); del a[i:i + 2]
        j = a.index('-c'); src = a[j + 1]; del a[j:j + 2]
        open(script, 'w').write('cd %s && %s -S -o "$1" %s\n' % (
            shlex.quote(e['directory']), ' '.join(shlex.quote(x) for x in a), shlex.quote(src)))
        break
else:
    sys.exit('no compile command for lib/deflate_decompress.c')
EOF
bash "$W/to_s.sh" "$W/dd.s"
COLD=$(nm -m "$W/b-test/$OBJREL" | awk '/\[cold func\]/ { print $NF }')

# mkvariant NAME K: the PR's dylib with FUNC at K (or "none") past a 128-byte
# boundary, in $W/v-NAME.
mkvariant() {
	python3 "$HERE/padasm.py" "$W/dd.s" "$W/dd-$1.s" "$FUNC" "$2" $COLD
	rm -rf "$W/v-$1"
	mkdir -p "$W/v-$1"
	cp -R "$W/b-test/CMakeFiles" "$W/v-$1/"
	cc -arch arm64 -c "$W/dd-$1.s" -o "$W/v-$1/$OBJREL"
	(cd "$W/v-$1" && sh CMakeFiles/libdeflate_shared.dir/link.txt)
}
code() {  # code LIB: FUNC's instructions without addresses
	otool -arch arm64 -tV -p "$FUNC" "$1" | python3 "$HERE/loopinfo.py" --code "$FUNC"
}
loop() {  # loop LIB: FUNC's start and main loop head within a 128-byte line
	otool -arch arm64 -tV -p "$FUNC" "$1" | python3 "$HERE/loopinfo.py" "$FUNC"
}

mkvariant ctl none
if ! same_text "$W/v-ctl/libdeflate.0.dylib" "$LT"; then
	echo "CONTROL FAILED: reassembling without padding does not reproduce CMake's dylib"
	exit 1
fi
LIBS=("$LB" "$LT" "$LC")
NAMES="0=master 1=pr 2=copy"
HEADS="master $(loop "$LB" | sed 's/.*head=//') pr $(loop "$LT" | sed 's/.*head=//')"
n=3
for k in $KS; do
	mkvariant "k$k" "$k"
	V=$W/v-k$k/libdeflate.0.dylib
	info=$(loop "$V")
	case "$info" in *"(+$k)"*) ;; *) echo "VARIANT k$k MISPLACED: $info"; exit 1 ;; esac
	if ! cmp -s <(code "$V") <(code "$LT"); then
		echo "VARIANT k$k HAS DIFFERENT CODE"
		exit 1
	fi
	LIBS+=("$V")
	NAMES="$NAMES $n=k$k"
	HEADS="$HEADS k$k ${info#*head=}"
	n=$((n + 1))
done

{
	echo "image: ${IMAGE:-?}"
	echo "os: macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
	echo "xcode: $(xcodebuild -version 2> /dev/null | tr '\n' ' ' | sed 's/Build version/build/; s/ *$//') at $(xcode-select -p 2> /dev/null)"
	echo "cpu: $(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.ncpu) CPUs, $(( $(sysctl -n hw.memsize) >> 30 )) GiB, cache line $(sysctl -n hw.cachelinesize)"
	echo "cc: $(cc --version | head -1)"
	echo "base: $(git -C "$TOP" log -1 --format='%h %s' "$BASE_REV")"
	echo "test: $(git -C "$TOP" log -1 --format='%h %s' "$TEST_REV")"
	echo "libs: $NAMES"
	echo "heads: $HEADS"
	echo "control: reassembled PR dylib identical to CMake's; variants k0..k112 have the PR's fastloop instructions at the stated offsets"
	echo "fastloop: master $(fastloops "$LB") pr $(fastloops "$LT")"
	echo "inputs: $(ls "$IN/real" | tr '\n' ' ')"
	head -3 "$IN/MANIFEST.txt" | sed 's/^/manifest: /'
} | tee "$META"

for l in $DECOMP; do
	"$W/abp" -l "$l" -r 1 "${LIBS[@]}" -- "$IN"/real/* > /dev/null
	"$W/abp" -d -l "$l" -r 1 "$LT" "${LIBS[@]:3}" -- "$IN"/real/* > /dev/null
done
echo "identity: same compressed output and decompression cross-checked at levels $(echo $DECOMP | tr ' ' ',')" | tee -a "$META"

solo() {  # solo TAG MODE LEVEL ROUNDS REPS LIB... (as in time.sh)
	local tag=$1 mode=$2 level=$3 rounds=$4 reps=$5 d= r i k j t0
	shift 5
	local libs=("$@")
	local nl=${#libs[@]}
	local files=("$IN"/real/*)
	[ "$mode" = d ] && d=-d
	t0=$SECONDS
	for ((r = 0; r < rounds; r++)); do
		for ((i = 0; i < ${#files[@]}; i++)); do
			for ((k = 0; k < nl; k++)); do
				j=$(((k + r + i) % nl))
				echo "=== $tag $mode $level round $r lib $j" >> "$RES"
				"$W/abp" $d -l "$level" -r "$reps" "${libs[$j]}" -- "${files[$i]}" >> "$RES"
			done
		done
	done
	echo "sweep $mode L$level: $((SECONDS - t0))s"
}
for l in $DECOMP; do
	solo sweep d "$l" "${AB_ROUNDS_D:-8}" "${AB_REPS_D:-11}" "${LIBS[@]}"
done
echo "ALL DONE" | tee -a "$META"
python3 "$HERE/sweep_summary.py" "$OUT"
