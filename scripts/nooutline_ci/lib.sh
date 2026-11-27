# nooutline_ci lib.sh - test-only helpers sourced by probe.sh and time.sh.
# Needs $TOP (the repository) and $W (a work directory).  Works on macOS and
# Linux; the arm64-specific Mach-O helpers fall back to ELF tools on Linux.

ncpu() {
	getconf _NPROCESSORS_ONLN 2> /dev/null || sysctl -n hw.ncpu
}

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
	       cmake --build "$b" -j"$(ncpu)"; } > "$b.log" 2>&1; then
		echo "BUILD FAILED: $b"
		tail -30 "$b.log"
		return 1
	fi
}

# dylib BDIR: the shared library file itself (not a symlink).
dylib() {
	local f=$1/libdeflate.dylib
	[ -e "$f" ] || f=$1/libdeflate.so
	python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$f"
}

is_macho() {
	[ "$(uname -s)" = Darwin ]
}

# outlined LIB [ARCH]: number of machine-outliner functions in LIB.
outlined() {
	if [ -n "${2:-}" ]; then
		nm -arch "$2" "$1" | grep -c 'OUTLINED_FUNCTION' || true
	else
		nm "$1" | grep -c 'OUTLINED_FUNCTION' || true
	fi
}

# textsize LIB: size of the (arm64) text section in bytes.
textsize() {
	if is_macho; then
		size -m -arch arm64 "$1" | awk '/Section __text:/ { print $3; exit }'
	else
		size -A "$1" | awk '$1 == ".text" { print $2; exit }'
	fi
}

# text LIB: the (arm64) text section's bytes, as text, on stdout.
text() {
	if is_macho; then
		otool -arch arm64 -t "$1" | tail -n +2
	else
		objcopy -O binary --only-section=.text "$1" /dev/stdout | od -An -tx1 -v
	fi
}

# same_text LIB1 LIB2: true if the two (arm64) text sections are identical.
same_text() {
	cmp -s <(text "$1") <(text "$2")
}

# fastloops LIB: "name=address" of the fastloop functions in LIB, with each
# address's offset from a 64-byte boundary.
fastloops() {
	nm "$1" | awk '/decompress_fastloop_(un)?checked/ { print $3, $1 }' |
		while read -r name addr; do
			printf '%s=0x%s(+%d) ' "${name#_}" "$addr" $((16#$addr % 64))
		done
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
