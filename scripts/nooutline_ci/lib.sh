# nooutline_ci lib.sh - test-only helpers sourced by probe.sh and time.sh.
# Needs $TOP (the repository) and $W (a work directory).

# tree NAME REV: REV's files in $W/src-NAME (git archive: no local changes).
tree() {
	if [ ! -d "$W/src-$1" ]; then
		mkdir -p "$W/src-$1"
		git -C "$TOP" archive "$2" | tar -x -C "$W/src-$1"
	fi
}

# build SRC BDIR CMAKE_ARGS...: configure and build; the log is BDIR.log.
build() {
	local src=$1 b=$2
	shift 2
	rm -rf "$b"
	if ! { cmake -S "$src" -B "$b" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON "$@" &&
	       cmake --build "$b" -j"$(sysctl -n hw.ncpu)"; } > "$b.log" 2>&1; then
		echo "BUILD FAILED: $b"
		tail -30 "$b.log"
		return 1
	fi
}

# dylib BDIR: the shared library file itself (not a symlink).
dylib() {
	python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$1/libdeflate.dylib"
}

# outlined LIB [ARCH]: number of machine-outliner functions in LIB.
outlined() {
	if [ -n "${2:-}" ]; then
		nm -arch "$2" "$1" | grep -c 'OUTLINED_FUNCTION' || true
	else
		nm "$1" | grep -c 'OUTLINED_FUNCTION' || true
	fi
}

# textsize LIB: size of the arm64 __TEXT,__text section in bytes.
textsize() {
	size -m -arch arm64 "$1" | awk '/Section __text:/ { print $3; exit }'
}

# same_text LIB1 LIB2: true if the two arm64 __text sections are identical.
same_text() {
	cmp -s <(otool -arch arm64 -t "$1" | tail -n +2) <(otool -arch arm64 -t "$2" | tail -n +2)
}

# flagcount BDIR: "N/M" = compile commands with -mno-outline / all.
flagcount() {
	echo "$(grep -c -e '-mno-outline' "$1/compile_commands.json" || true)/$(grep -c '"file"' "$1/compile_commands.json" || true)"
}

# cachevar BDIR VAR: VAR's value in BDIR's CMake cache.
cachevar() {
	sed -n "s/^$2:[A-Z]*=//p" "$1/CMakeCache.txt"
}

# warnings BDIR: number of compiler warnings in BDIR's build log.
warnings() {
	grep -c 'warning:' "$1.log" || true
}
