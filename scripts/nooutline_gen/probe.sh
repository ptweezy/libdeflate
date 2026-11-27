#!/bin/bash
# nooutline_gen probe.sh - test-only, not for upstream.
# HEAD~2 = upstream master, HEAD~1 = PR 487 with the review feedback applied.
# Checks that commit with the multi-config CMake generators:
#   - Xcode generator, with the generator expression exactly as suggested in
#     the review (no Xcode exception): does CMake accept it?
#   - Xcode generator, revised commit and master: Release and MinSizeRel
#     builds, which sources get -mno-outline, code size
#   - Ninja Multi-Config, master, revised commit, and as-suggested variant:
#     the same
#   - the test programs under both generators
# usage: probe.sh OUTDIR
#   XCODE=/Applications/Xcode_N.app   use this Xcode instead of the default
#   SKIP_NINJA=1                      skip the Ninja Multi-Config parts
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: probe.sh OUTDIR}
W=$OUT/w
rm -rf "$W"
mkdir -p "$W"
RES=$OUT/probe.txt
: > "$RES"
FAIL=0
say() { echo "$@" | tee -a "$RES"; }
failed() { say "  $1 FAILED"; tail -25 "$2" 2> /dev/null; FAIL=1; }

if [ -n "${XCODE:-}" ] && [ -d "$XCODE/Contents/Developer" ]; then
	export DEVELOPER_DIR=$XCODE/Contents/Developer
fi
say "== image ${IMAGE:-?}: macOS $(sw_vers -productVersion), $(sysctl -n machdep.cpu.brand_string)"
say "$(xcodebuild -version 2>&1 | tr '\n' ' ' | sed 's/ *$//'); $(cc --version 2>&1 | head -1)"
say "$(cmake --version | head -1); ninja $(ninja --version 2>&1 | head -1)"
say "master  = $(git -C "$TOP" log -1 --format='%h %s' HEAD~2)"
say "revised = $(git -C "$TOP" log -1 --format='%h %s' HEAD~1)"

tree() { mkdir -p "$W/src-$1" && git -C "$TOP" archive "$2" | tar -x -C "$W/src-$1"; }
tree base HEAD~2 || exit 1
tree pr HEAD~1 || exit 1
# The snippet as suggested in the review: the same file without "AND NOT XCODE".
cp -R "$W/src-pr" "$W/src-asis"
sed -i '' 's/ VERSION_GREATER_EQUAL 3\.11 AND NOT XCODE)/ VERSION_GREATER_EQUAL 3.11)/' "$W/src-asis/CMakeLists.txt"
if cmp -s "$W/src-pr/CMakeLists.txt" "$W/src-asis/CMakeLists.txt"; then
	say "ERROR: could not derive the as-suggested variant"
	exit 1
fi
name() { case $1 in base) echo master ;; pr) echo revised ;; asis) echo as-suggested ;; esac; }

LIBONLY=(-DLIBDEFLATE_BUILD_STATIC_LIB=OFF -DLIBDEFLATE_BUILD_GZIP=OFF)
ALLCFG='-DCMAKE_CONFIGURATION_TYPES=Debug;Release;MinSizeRel;RelWithDebInfo'
conf() { # generator src builddir [cmake args]
	local g=$1 s=$2 b=$3
	shift 3
	rm -rf "$b"
	cmake -G "$g" -S "$s" -B "$b" "$@" > "$b.cfg.log" 2>&1
}
bld() { # builddir config [-v]
	cmake --build "$1" --config "$2" ${3:-} > "$1-$2.log" 2>&1
}
dylib() { find "$1" -path "*/$2/*" -name 'libdeflate*.dylib' -type f | head -1; }
textsize() { size -m "$1" | awk '/Section __text:/ {s += $3} END {print s + 0}'; }
texthash() { otool -t "$1" | sed 1d | shasum | cut -c1-12; }
outlined() { nm "$1" 2> /dev/null | grep -c OUTLINED_FUNCTION; true; }
flagfiles() { # build log -> the sources compiled with -mno-outline
	local f
	f=$(grep -- '-mno-outline' "$1" | grep -E -o -- ' -c [^ ]+\.c' | awk '{print $2}' | xargs -n1 basename 2> /dev/null | sort -u | tr '\n' ' ' | sed 's/ $//')
	echo "${f:-none}"
}
ncomp() { grep -E -o -- ' -c [^ ]+\.c' "$1" | sort -u | wc -l | tr -d ' '; }
pbxflag() { # project.pbxproj -> the sources whose per-file flags have -mno-outline
	local f
	f=$(grep -E 'isa = PBXBuildFile;.*COMPILER_FLAGS = [^;]*-mno-outline' "$1" | sed -E 's|^[^/]*/\* ([^*]*) \*/.*|\1|' | xargs -n1 basename 2> /dev/null | sort -u | tr '\n' ' ' | sed 's/ $//')
	echo "${f:-none}"
}
report() { # tree builddir config
	local lib log=$2-$3.log
	lib=$(dylib "$2" "$3")
	if [ -z "$lib" ]; then
		say "  $(name "$1") $3: no dylib found"
		FAIL=1
		return
	fi
	say "  $(name "$1") $3: -mno-outline on: $(flagfiles "$log") (of $(ncomp "$log") sources in the log); __text $(textsize "$lib"), sha $(texthash "$lib"); OUTLINED_FUNCTIONs $(outlined "$lib"); warnings $(grep -c 'warning:' "$log") [${lib#"$2"/}]"
}
compare() { # prefix treeA treeB config
	local a b sa sb
	a=$(dylib "$W/$1-$2" "$4")
	b=$(dylib "$W/$1-$3" "$4")
	[ -n "$a" ] && [ -n "$b" ] || return
	if [ "$(texthash "$a")" = "$(texthash "$b")" ]; then
		say "  $4: $(name "$3") code is identical to $(name "$2")"
	else
		sa=$(textsize "$a")
		sb=$(textsize "$b")
		say "  $4: $(name "$3") code differs from $(name "$2"): __text $sa -> $sb ($(printf '%+d' $((sb - sa))) bytes)"
	fi
}

XGEN=${XGEN:-Xcode}
NGEN=${NGEN:-Ninja Multi-Config}

say
say "$XGEN generator, generator expression as suggested in the review (no Xcode exception):"
b=$W/x-asis
if conf "$XGEN" "$W/src-asis" "$b" "${LIBONLY[@]}"; then
	say "  configure succeeded"
	for c in Release MinSizeRel; do
		if bld "$b" $c; then report asis "$b" $c; else failed "as-suggested $c build" "$b-$c.log"; fi
	done
else
	say "  configure failed:"
	grep -A7 'CMake Error' "$b.cfg.log" | head -40 | sed "s|$W/||g; s/^/    /" | tee -a "$RES"
fi

say
say "$XGEN generator:"
for t in base pr; do
	b=$W/x-$t
	if ! conf "$XGEN" "$W/src-$t" "$b" "${LIBONLY[@]}"; then
		failed "$(name $t) configure" "$b.cfg.log"
		continue
	fi
	for c in Release MinSizeRel; do
		if bld "$b" $c; then report $t "$b" $c; else failed "$(name $t) $c build" "$b-$c.log"; fi
	done
	p=$b/libdeflate.xcodeproj/project.pbxproj
	[ -f "$p" ] && say "  $(name $t): per-file -mno-outline in project.pbxproj: $(pbxflag "$p") ($(grep -c -- '-mno-outline' "$p") lines mention the flag)"
done
compare x base pr Release
compare x base pr MinSizeRel

if [ -z "${SKIP_NINJA:-}" ]; then
	say
	say "$NGEN generator:"
	for t in base pr asis; do
		b=$W/n-$t
		if ! conf "$NGEN" "$W/src-$t" "$b" "${LIBONLY[@]}" "$ALLCFG"; then
			failed "$(name $t) configure" "$b.cfg.log"
			continue
		fi
		for c in Release MinSizeRel; do
			if bld "$b" $c -v; then report $t "$b" $c; else failed "$(name $t) $c build" "$b-$c.log"; fi
		done
	done
	compare n base pr Release
	compare n base pr MinSizeRel
	compare n pr asis Release
	compare n pr asis MinSizeRel
fi

say
say "Test programs, revised commit (static and shared libraries, gzip, tests):"
tests() { # label generator builddir -v|'' [cmake args]
	local l=$1 g=$2 b=$3 v=$4 c rc
	shift 4
	if ! conf "$g" "$W/src-pr" "$b" -DLIBDEFLATE_BUILD_TESTS=ON "$@"; then
		failed "$l configure" "$b.cfg.log"
		return
	fi
	for c in Release MinSizeRel; do
		if ! bld "$b" $c $v; then
			failed "$l $c build" "$b-$c.log"
			continue
		fi
		ctest --test-dir "$b" -C $c > "$b-$c.ctest" 2>&1
		rc=$?
		[ $rc = 0 ] || FAIL=1
		say "  $l $c: -mno-outline on: $(flagfiles "$b-$c.log"); warnings $(grep -c 'warning:' "$b-$c.log"); ctest $([ $rc = 0 ] && echo pass || echo FAIL), $(grep -E 'tests passed|tests failed' "$b-$c.ctest" | tail -1)"
	done
}
tests "$XGEN" "$XGEN" "$W/x-test" ''
if [ -z "${SKIP_NINJA:-}" ]; then
	tests "$NGEN, -Werror and assertions" "$NGEN" "$W/n-test" -v "$ALLCFG" "-DCMAKE_C_FLAGS=-Werror -DLIBDEFLATE_ENABLE_ASSERTIONS"
fi
say
say "PROBE DONE (failures: $FAIL)"
exit $FAIL
