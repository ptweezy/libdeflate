#!/bin/bash
# nooutline_ci probe.sh - test-only: for each Xcode on this runner image, check
# PR 487 (HEAD~1) against upstream master (HEAD~2) without timing anything:
#   - is -mno-outline detected and passed (Release), and not passed (MinSizeRel)
#   - how many machine-outliner functions master's dylib has, and the PR's
#   - __text sizes
#   - does the PR configured with -DHAVE_MNO_OUTLINE=OFF give master's code,
#     and is MinSizeRel unchanged
#   - universal (arm64;x86_64) build, compiler warnings, ctest
# usage: probe.sh OUTDIR    (XCODES="/Applications/Xcode_A.app ..." to limit)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: probe.sh OUTDIR}
W=$OUT/w
mkdir -p "$W"
. "$HERE/lib.sh"
RES=$OUT/probe.txt
tree base HEAD~2
tree pr HEAD~1
{
	echo "== image ${IMAGE:-?}: macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion)), $(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.ncpu) CPUs"
	echo "master = $(git -C "$TOP" log -1 --format='%h %s' HEAD~2)"
	echo "PR     = $(git -C "$TOP" log -1 --format='%h %s' HEAD~1)"
	echo "default Xcode: $(xcode-select -p)"
} | tee "$RES"

if [ -z "${XCODES:-}" ]; then
	XCODES=$(for x in /Applications/Xcode*.app; do [ -L "$x" ] || echo "$x"; done | sort -V)
fi
REL=(-DCMAKE_BUILD_TYPE=Release -DLIBDEFLATE_BUILD_STATIC_LIB=OFF -DLIBDEFLATE_BUILD_GZIP=OFF)
MSR=(-DCMAKE_BUILD_TYPE=MinSizeRel -DLIBDEFLATE_BUILD_STATIC_LIB=OFF -DLIBDEFLATE_BUILD_GZIP=OFF)
seen=
for x in $XCODES; do
	export DEVELOPER_DIR=$x/Contents/Developer
	if ! v=$(cc --version 2> /dev/null | head -1); then
		echo "$(basename "$x"): no working compiler" | tee -a "$RES"
		continue
	fi
	xv=$(xcodebuild -version 2> /dev/null | tr '\n' ' ' | sed 's/Build version/build/; s/ *$//')
	case "$seen" in *"|$v|"*)
		echo "$(basename "$x") ($xv): same compiler as above ($v), skipped" | tee -a "$RES"
		continue ;;
	esac
	seen="$seen|$v|"
	t=$W/$(basename "$x" .app)
	ok=1
	build "$W/src-base" "$t-base" "${REL[@]}" || ok=0
	build "$W/src-pr" "$t-pr" -DCMAKE_BUILD_TYPE=Release -DLIBDEFLATE_BUILD_TESTS=ON || ok=0
	build "$W/src-pr" "$t-off" "${REL[@]}" -DHAVE_MNO_OUTLINE=OFF || ok=0
	build "$W/src-base" "$t-msrbase" "${MSR[@]}" || ok=0
	build "$W/src-pr" "$t-msr" "${MSR[@]}" || ok=0
	build "$W/src-pr" "$t-uni" "${REL[@]}" '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64' || ok=0
	if [ $ok = 0 ]; then
		echo "$(basename "$x") ($xv): BUILD FAILED, see above" | tee -a "$RES"
		continue
	fi
	lb=$(dylib "$t-base"); lp=$(dylib "$t-pr"); lo=$(dylib "$t-off")
	lmb=$(dylib "$t-msrbase"); lm=$(dylib "$t-msr"); lu=$(dylib "$t-uni")
	sb=$(textsize "$lb"); sp=$(textsize "$lp")
	if same_text "$lb" "$lo"; then off=yes; else off=NO; fi
	if same_text "$lmb" "$lm"; then msr=yes; else msr=NO; fi
	if same_text "$lb" "$lp"; then same="IDENTICAL to master (the flag changes nothing)"; else same="differs from master"; fi
	if ctest --test-dir "$t-pr" > "$t-pr.ctest" 2>&1; then ct=pass; else ct=FAIL; fi
	{
		echo
		echo "$(basename "$x") ($xv): $v"
		echo "  HAVE_MNO_OUTLINE=$(cachevar "$t-pr" HAVE_MNO_OUTLINE); -mno-outline in $(flagcount "$t-pr") compile commands (Release+tests), $(flagcount "$t-msr") (MinSizeRel), $(flagcount "$t-uni") (universal), $(flagcount "$t-base") (master)"
		echo "  OUTLINED_FUNCTIONs in libdeflate.dylib: master $(outlined "$lb"), PR $(outlined "$lp") (Release); master $(outlined "$lmb"), PR $(outlined "$lm") (MinSizeRel); universal PR arm64 $(outlined "$lu" arm64), x86_64 $(outlined "$lu" x86_64)"
		echo "  __text: master $sb, PR $sp ($(python3 -c "import sys; a, b = int(sys.argv[1]), int(sys.argv[2]); print('%+d bytes, %+.1f%%' % (b - a, 100.0 * (b - a) / a))" "$sb" "$sp")); PR code $same"
		echo "  PR with -DHAVE_MNO_OUTLINE=OFF has master's code: $off; MinSizeRel code unchanged: $msr"
		echo "  universal archs: $(lipo -archs "$lu"); compiler warnings: Release+tests $(warnings "$t-pr"), MinSizeRel $(warnings "$t-msr"), universal $(warnings "$t-uni")"
		echo "  ctest (Release+tests): $ct, $(grep -E 'tests passed|tests failed' "$t-pr.ctest" | tail -1)"
	} | tee -a "$RES"
done

# The Xcode generator (multi-config: the flag comes from a $<CONFIG> generator
# expression), with the image's default Xcode.
unset DEVELOPER_DIR
b=$W/xcodegen
echo | tee -a "$RES"
if cmake -G Xcode -S "$W/src-pr" -B "$b" -DLIBDEFLATE_BUILD_STATIC_LIB=OFF -DLIBDEFLATE_BUILD_GZIP=OFF > "$b.log" 2>&1 &&
   cmake --build "$b" --config Release > "$b-rel.log" 2>&1 &&
   cmake --build "$b" --config MinSizeRel > "$b-msr.log" 2>&1; then
	rel=$(find "$b" -path '*Release*' ! -path '*MinSizeRel*' -name 'libdeflate*.dylib' -type f | head -1)
	msr=$(find "$b" -path '*MinSizeRel*' -name 'libdeflate*.dylib' -type f | head -1)
	# Configurations whose build settings in the generated project pass the flag.
	cfgs=$(python3 - "$b/libdeflate.xcodeproj/project.pbxproj" <<-'EOF'
	import re, sys
	s = open(sys.argv[1]).read()
	pat = r'isa = XCBuildConfiguration;\s*buildSettings = \{(.*?)\};\s*name = ("?)([A-Za-z]+)\2;'
	print(' '.join(sorted({m.group(3) for m in re.finditer(pat, s, re.S) if '-mno-outline' in m.group(1)})) or 'none')
	EOF
	)
	echo "Xcode generator, default Xcode ($(xcodebuild -version | head -1)): configurations with -mno-outline: $cfgs; OUTLINED_FUNCTIONs Release $(outlined "$rel"), MinSizeRel $(outlined "$msr")" | tee -a "$RES"
else
	echo "Xcode generator build FAILED" | tee -a "$RES"
	tail -20 "$b.log" "$b-rel.log" "$b-msr.log" 2> /dev/null || true
fi
echo "PROBE DONE" | tee -a "$RES"
