#!/bin/bash
# Check that a series of commits doesn't change libdeflate's output on this
# platform (test-only; not for upstream).
#
# The branch is BASE + N series commits (default N = 3) + the commit that
# adds this script, so the trees are: base = HEAD~(N+1), c1 = HEAD~N, ...,
# cN = HEAD~1.  The identity tools in this directory are built against every
# tree and run on the same inputs; then every candidate's output is compared
# with base's.  A task is IDENTICAL only if the outputs are byte-equal, all
# exit codes are 0 and nothing was written to stderr.
#
# Environment (all optional):
#   R8_NAME      name of this platform configuration (default: local)
#   R8_CC        compiler (default: cc); "cl" selects the MSVC command line
#   R8_CFLAGS    compiler flags (default: -O2)
#   R8_RUN       command that runs a binary, e.g. "qemu-s390x-static"
#                (default: none); "auto:CMD" uses CMD only if the binaries
#                don't run natively, and then installs qemu-user-static if
#                CMD is missing (so that the native attempt can't have been
#                redirected to qemu by binfmt_misc)
#   R8_LEVELS    level groups, e.g. "0,1,2,3 4,5,6,7" (default: all 13 levels
#                in 6 groups); each group is one process per tool
#   R8_UNIT      1 = also run the unit tests hut and utd (default: 1)
#   R8_PROFILE   core = the tasks that every platform runs; full = core plus
#                the tasks that are too slow under emulation; auto (default) =
#                full if the binaries run natively
#   R8_NCOMMITS  N (default: 3)
#   R8_INPUTS    r8-inputs.tar made by the "inputs" job (default: make the
#                inputs here with mkin.py; R8_CORPUS is passed to it)
#   R8_WORK      work directory (default: $RUNNER_TEMP/r8_ident)
#   R8_JOBS      parallel processes (default: number of CPUs)
#
# Results are left in $R8_WORK/artifact/$R8_NAME: verdict.tsv, meta.txt,
# base/ (base's outputs), and diff/ and err/ for anything not IDENTICAL.
# Exit status: 0 if everything is IDENTICAL, else 1.

set -u
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)

NAME=${R8_NAME:-local}
CC=${R8_CC:-cc}
CFLAGS=${R8_CFLAGS:--O2}
RUN=${R8_RUN:-}
LGROUPS=${R8_LEVELS:-0,1,2,3 4,5,6,7 8,9 10 11 12}
UNIT=${R8_UNIT:-1}
PROFILE=${R8_PROFILE:-auto}
NC=${R8_NCOMMITS:-3}
JOBS=${R8_JOBS:-$(getconf _NPROCESSORS_ONLN 2> /dev/null || nproc 2> /dev/null || echo 4)}
W=${R8_WORK:-${RUNNER_TEMP:-/tmp}/r8_ident}

WIN=0
case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*)
	# Use C:/... paths, which both the MSYS and the native tools accept,
	# and don't let MSYS rewrite arguments such as -FoC:/dir/file.obj.
	WIN=1
	W=$(cygpath -m "$W")
	HERE=$(cygpath -m "$HERE")
	TOP=$(cygpath -m "$TOP")
	export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
	;;
esac
MSVC=0
EXE=
case "$CC" in
cl|cl.exe) MSVC=1 ;;
esac
[ $WIN = 1 ] && EXE=.exe

START=$(date +%s)
rm -rf "$W/src" "$W/obj" "$W/bin" "$W/out" "$W/tasks" "$W/log" "$W/artifact"
mkdir -p "$W/src" "$W/obj" "$W/bin" "$W/out" "$W/tasks" "$W/log"
A=$W/artifact/$NAME
mkdir -p "$A/base" "$A/diff" "$A/err"
META=$A/meta.txt
: > "$META"
meta() { echo "$*" | tee -a "$META"; }

meta "name=$NAME"
meta "uname=$(uname -srm)"
meta "cc=$CC cflags=$CFLAGS"
meta "levels=$LGROUPS unit=$UNIT ncommits=$NC jobs=$JOBS"

# ---- inputs ----
if [ -n "${R8_INPUTS:-}" ]; then
	mkdir -p "$W/in.tmp" && rm -rf "$W/in" &&
		tar -xf - -C "$W/in.tmp" < "$R8_INPUTS" &&
		mv "$W/in.tmp/in" "$W/in" && rmdir "$W/in.tmp" || exit 1
elif [ ! -f "$W/in/LIST" ]; then
	python3 "$HERE/mkin.py" "$W/in" ${R8_CORPUS:+"$R8_CORPUS"} > /dev/null || exit 1
fi
cp "$W/in/MANIFEST" "$A/inputs-MANIFEST.txt"
FILES=$(sed 's|^|in/|' "$W/in/LIST" | tr '\n' ' ')
FLVFILES=$(sed 's|^|in/|' "$W/in/FLVLIST" | tr '\n' ' ')
meta "inputs=$(wc -l < "$W/in/LIST" | tr -d ' ') files, list sha256 below"
grep '^#' "$W/in/MANIFEST" >> "$META"

# ---- trees ----
TREES=base
i=1
while [ $i -le $NC ]; do
	TREES="$TREES c$i"
	i=$((i + 1))
done
CANDS=${TREES#base}
i=0
for t in $TREES; do
	rev=HEAD~$((NC + 1 - i))
	mkdir -p "$W/src/$t"
	git -C "$TOP" -c core.autocrlf=false archive "$rev" |
		tar -xf - -C "$W/src/$t" || exit 1
	meta "tree $t = $(git -C "$TOP" log --format='%h %s' -1 "$rev")"
	if [ $i != 0 ]; then
		meta "  files changed vs base: $(git -C "$TOP" diff --name-only \
			"HEAD~$((NC + 1))" "$rev" | tr '\n' ' ')"
	fi
	i=$((i + 1))
done

# ---- build ----
LIBTOOLS="probe xcheck mdrv vid flvtest"
UNITTOOLS="hut utd"
build_tree() {
	t=$1
	o=$W/obj/$t
	mkdir -p "$o"
	cd "$W/src/$t" || return 1
	objs=
	objs_nodc=
	for f in lib/*.c lib/*/*.c; do
		n=$(echo "$f" | tr '/.' '__')
		if [ $MSVC = 1 ]; then
			ob=$o/$n.obj
			"$CC" -nologo -c $CFLAGS -I. "-Fo$ob" "$f" || return 1
		else
			ob=$o/$n.o
			"$CC" $CFLAGS -std=gnu11 -w -I. -c "$f" -o "$ob" || return 1
		fi
		objs="$objs $ob"
		[ "$f" = lib/deflate_compress.c ] || objs_nodc="$objs_nodc $ob"
	done
	for tool in $LIBTOOLS $UNITTOOLS; do
		case " $UNITTOOLS " in
		*" $tool "*) lo=$objs_nodc ;;	# these include deflate_compress.c
		*) lo=$objs ;;
		esac
		if [ $MSVC = 1 ]; then
			"$CC" -nologo $CFLAGS -I. "-Fo$o/tool_$tool.obj" \
				"-Fe$W/bin/$tool-$t.exe" "$HERE/$tool.c" $lo || return 1
		else
			"$CC" $CFLAGS -std=gnu11 -w -I. -o "$W/bin/$tool-$t$EXE" \
				"$HERE/$tool.c" $lo || return 1
		fi
	done
}
for t in $TREES; do
	( build_tree $t > "$W/log/build-$t.log" 2>&1
	  echo $? > "$W/log/build-$t.rc" ) &
done
wait
BUILD_OK=1
for t in $TREES; do
	if [ "$(cat "$W/log/build-$t.rc")" != 0 ]; then
		BUILD_OK=0
		echo "::error::$NAME: build of $t failed"
		tail -40 "$W/log/build-$t.log"
		cp "$W/log/build-$t.log" "$A/err/"
	fi
done
if [ $BUILD_OK = 0 ]; then
	printf '%s\tBUILD\t0\tBUILD-FAILED\n' "$NAME" > "$A/verdict.tsv"
	exit 1
fi
meta "built in $(( $(date +%s) - START ))s"

# ---- how to run the binaries ----
cd "$W" || exit 1
case "$RUN" in
auto:*)
	meta "binfmt_misc handlers: $(ls /proc/sys/fs/binfmt_misc 2> /dev/null | tr '\n' ' ')"
	if "bin/probe-base$EXE" 2> /dev/null | grep -q '^probe-ok$'; then
		RUN=
		meta "exec=native (the binaries run without ${R8_RUN#auto:})"
	else
		RUN=${RUN#auto:}
		meta "exec=$RUN (the binaries don't run natively)"
		if ! command -v "${RUN%% *}" > /dev/null; then
			sudo apt-get install -y qemu-user-static > "$W/log/apt.log" 2>&1 ||
				tail -20 "$W/log/apt.log"
		fi
	fi
	;;
"") meta "exec=native" ;;
*) meta "exec=$RUN" ;;
esac
if ! $RUN "bin/probe-base$EXE" > "$W/log/probe.txt" 2> "$W/log/probe.err" ||
   ! grep -q '^probe-ok' "$W/log/probe.txt"; then
	echo "::error::$NAME: the probe program doesn't run"
	cat "$W/log/probe.txt" "$W/log/probe.err"
	cat "$W/log/probe.txt" "$W/log/probe.err" > "$A/err/probe.txt"
	printf '%s\tPROBE\t0\tRUN-FAILED\n' "$NAME" > "$A/verdict.tsv"
	exit 1
fi
tr -d '\r' < "$W/log/probe.txt" | grep -v '^probe-ok' | sed 's/^/probe: /' |
	tee -a "$META"
if [ -n "$RUN" ]; then
	meta "runner: $(${RUN%% *} --version 2> /dev/null | head -1)"
fi
if [ $MSVC = 1 ]; then
	meta "compiler: $("$CC" 2>&1 | head -1 | tr -d '\r')"
else
	meta "compiler: $("$CC" --version 2> /dev/null | head -1)"
fi
if [ "$PROFILE" = auto ]; then
	if [ -z "$RUN" ]; then PROFILE=full; else PROFILE=core; fi
fi
meta "profile=$PROFILE"

# ---- tasks ----
# add NAME "ENV" TOOL ARGS...: one process per tree
TASKS=
add() {
	tn=$1
	envs=$2
	tool=$3
	shift 3
	TASKS="$TASKS $tn"
	for t in $TREES; do
		mkdir -p "out/$t"
		f=tasks/$t.$tn.sh
		cat > "$f" <<EOF
cd '$W' || exit 1
s=\$(date +%s)
$envs $RUN bin/$tool-$t$EXE $* > out/$t/$tn.raw 2> out/$t/$tn.err
rc=\$?
echo \$rc > out/$t/$tn.rc
tr -d '\\r' < out/$t/$tn.raw > out/$t/$tn.txt
wc -l < out/$t/$tn.txt > out/$t/$tn.lines
rm -f out/$t/$tn.raw
e=\$(( \$(date +%s) - s ))
echo \$e > out/$t/$tn.sec
echo "  done $t.$tn rc=\$rc \${e}s"
EOF
		echo "$f" >> tasks/LIST
	done
}

# The slowest groups go first.
RGROUPS=
for g in $LGROUPS; do
	RGROUPS="$g $RGROUPS"
done
for g in $RGROUPS; do
	gn=L$(echo "$g" | tr ',' '_')
	if [ $PROFILE = full ]; then
		add "flv.$gn.mid2" "" flvtest mid2 "$g" $FLVFILES
		add "flv.$gn.big" "" flvtest big "$g" $FLVFILES
	fi
	add "xcheck.$gn" "" xcheck -l "$g" $FILES
	add "flv.$gn.mid" "" flvtest mid "$g" $FLVFILES
	for chunk in 1024 333 4096; do
		add "mdrv.$gn.c$chunk" "" mdrv "$g" $chunk $FILES
	done
	for seed in 13 17; do
		for garb in 0 7; do
			add "vid.$gn.s$seed.g$garb.n250" "GARB=$garb MAXLEN=131072" \
				vid $seed 250 "$g" $FILES
		done
	done
	add "flv.$gn.small" "" flvtest small "$g" $FLVFILES
	add "flv.$gn.wrap" "" flvtest wrap "$g" $FLVFILES
	if [ $PROFILE = full ]; then
		for chunk in 0 65536; do
			add "mdrv.$gn.c$chunk" "" mdrv "$g" $chunk $FILES
		done
		add "vid.$gn.s21.g7.n600" "GARB=7 MAXLEN=262144" \
			vid 21 600 "$g" $FILES
	fi
done
if [ "$UNIT" = 1 ]; then
	add "utd.s1" "" utd 400000 400000 1
	add "hut.s1" "" hut 1 1000000
	add "hut.s2" "" hut 2 1000000
fi

NT=$(wc -l < tasks/LIST | tr -d ' ')
echo "running $NT processes, $JOBS at a time"
T0=$(date +%s)
xargs -P "$JOBS" -n 1 bash < tasks/LIST
meta "ran in $(( $(date +%s) - T0 ))s"

# ---- compare ----
# (Shell builtins where possible: starting processes is slow on Windows.)
V=$A/verdict.tsv
: > "$V"
BAD=0
rd() {		# file: print its first line, or nothing
	x=
	[ -f "$1" ] && IFS= read -r x < "$1"
	echo "$x"
}
status() {	# tree task: sets st to ok if rc = 0 and stderr is empty
	x=
	[ -f "out/$1/$2.rc" ] && IFS= read -r x < "out/$1/$2.rc"
	if [ "$x" = 0 ] && [ ! -s "out/$1/$2.err" ]; then
		st=ok
	else
		st="rc=$x"
	fi
}
keep_err() {	# tree task
	{
		echo "== $1.$2 rc=$(rd "out/$1/$2.rc"), stderr:"
		head -c 4000 "out/$1/$2.err"
	} >> "$A/err/$2.txt"
	echo "::error::$NAME: $1.$2 rc=$(rd "out/$1/$2.rc") stderr: $(head -c 300 "out/$1/$2.err" | tr '\n' ' ')"
}
cp out/base/*.txt "$A/base/"
for tn in $TASKS; do
	lines=
	[ -f "out/base/$tn.lines" ] && read -r lines < "out/base/$tn.lines"
	status base "$tn"
	bs=$st
	[ "${lines:-0}" = 0 ] && bs=empty
	if [ "$bs" != ok ]; then
		BAD=1
		keep_err base "$tn"
	fi
	row="$NAME	$tn	${lines:-0}	$bs"
	secs=
	[ -f "out/base/$tn.sec" ] && read -r secs < "out/base/$tn.sec"
	for c in $CANDS; do
		status "$c" "$tn"
		cs=$st
		if ! cmp -s "out/base/$tn.txt" "out/$c/$tn.txt"; then
			v=DIFFERENT
			BAD=1
			cp "out/$c/$tn.txt" "$A/diff/$c.$tn.txt"
			diff "out/base/$tn.txt" "out/$c/$tn.txt" > "out/$c/$tn.diff"
			nd=$(grep -c '^[<>]' "out/$c/$tn.diff")
			head -2000 "out/$c/$tn.diff" > "$A/diff/$c.$tn.diff"
			echo "::error::$NAME: $c differs from base in $tn ($nd changed lines in the diff of $lines lines, first ones below)"
			head -8 "$A/diff/$c.$tn.diff"
			[ "$cs" = ok ] || keep_err "$c" "$tn"
		elif [ "$cs" != ok ] || [ "$bs" != ok ]; then
			v=ERROR
			BAD=1
			[ "$cs" = ok ] || keep_err "$c" "$tn"
		else
			v=IDENTICAL
		fi
		row="$row	$v"
		x=
		[ -f "out/$c/$tn.sec" ] && read -r x < "out/$c/$tn.sec"
		secs="$secs $x"
	done
	echo "$row	$secs" >> "$V"
done
rmdir "$A/diff" "$A/err" 2> /dev/null
meta "total_seconds=$(( $(date +%s) - START ))"

# ---- report ----
echo
echo "== $NAME: tasks, lines of base output, and verdicts"
awk -F'\t' '
{
	split($2, a, ".")
	tool = a[1]
	n[tool]++
	l[tool] += $3
	for (i = 5; i < NF; i++) {
		if ($i != "IDENTICAL")
			bad[tool, i]++
		if (i > nc)
			nc = i
	}
	if ($4 != "ok")
		bbad[tool]++
}
END {
	for (tool in n) {
		b = "ok"
		if (bbad[tool] > 0)
			b = "ERRORS"
		s = sprintf("%-8s %3d tasks %8d lines  base %s", tool, n[tool], l[tool], b)
		for (i = 5; i <= nc; i++) {
			v = "IDENTICAL"
			if (bad[tool, i] > 0)
				v = "NOT IDENTICAL (" bad[tool, i] " tasks)"
			s = s sprintf("  c%d %s", i - 4, v)
		}
		print s
	}
}' "$V" | sort | tee "$A/summary.txt"
if [ $BAD = 0 ]; then
	echo "== $NAME: ALL IDENTICAL"
else
	echo "== $NAME: NOT ALL IDENTICAL (see the errors above)"
fi
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
	{
		echo "### $NAME: $([ $BAD = 0 ] && echo all identical || echo NOT all identical)"
		echo '```'
		cat "$A/summary.txt"
		echo '```'
	} >> "$GITHUB_STEP_SUMMARY"
fi
exit $BAD
