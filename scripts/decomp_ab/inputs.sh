#!/bin/bash
# decomp_ab inputs.sh - test-only: fetch the benchmark corpus.
# usage: inputs.sh OUTDIR
#
# OUTDIR gets the 12 Silesia corpus files and enwik8, plus MANIFEST.txt saying
# where they came from and every file's SHA-256.  Sources, in order: the
# official silesia.zip, then a GitHub mirror of the same files; enwik8.zip from
# mattmahoney.net, then a mirror.  Fails if neither works.
set -euo pipefail
OUT=${1:?usage: inputs.sh OUTDIR}
RAW=$OUT/raw
SIL="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
mkdir -p "$RAW"
: > "$OUT/MANIFEST.txt"

get() {
	curl -fsSL --retry 3 --retry-delay 5 --connect-timeout 30 --max-time 600 -o "$2" "$1"
}

get_silesia() {
	if get https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip "$RAW/silesia.zip" &&
	   unzip -q -o "$RAW/silesia.zip" -d "$OUT"; then
		echo "corpus silesia: sun.aei.polsl.pl silesia.zip" >> "$OUT/MANIFEST.txt"
		return 0
	fi
	local f
	for f in $SIL; do
		get "https://github.com/MiloszKrajewski/SilesiaCorpus/raw/master/$f.zip" "$RAW/$f.zip" &&
			unzip -q -o "$RAW/$f.zip" -d "$OUT" || return 1
	done
	echo "corpus silesia: github.com/MiloszKrajewski/SilesiaCorpus" >> "$OUT/MANIFEST.txt"
}

get_enwik8() {
	local u
	for u in https://mattmahoney.net/dc/enwik8.zip https://data.deepai.org/enwik8.zip; do
		if get "$u" "$RAW/enwik8.zip" && unzip -q -o "$RAW/enwik8.zip" -d "$OUT"; then
			echo "corpus enwik8: $u" >> "$OUT/MANIFEST.txt"
			return 0
		fi
	done
	return 1
}

get_silesia
get_enwik8
rm -rf "$RAW"
(cd "$OUT" && sha256sum $SIL enwik8 >> MANIFEST.txt && ls -l)
cat "$OUT/MANIFEST.txt"
