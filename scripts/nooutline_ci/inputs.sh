#!/bin/bash
# nooutline_ci inputs.sh - test-only: the benchmark inputs.
# usage: inputs.sh OUTDIR
#
# OUTDIR/real  13 files of 2 MiB: slice.py's slice of each Silesia file and of
#              enwik8 (the "Silesia files cut to 2 MiB" of the PR description;
#              checked against slices.sha256)
# OUTDIR/MANIFEST.txt  where the corpus came from, and every file's SHA-256
#
# Corpus sources, in order: the official silesia.zip, then a GitHub mirror of
# the same files; enwik8.zip from mattmahoney.net, then a mirror.  Fails if
# neither works (no fallback corpus: the point is to compare with the PR).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: inputs.sh OUTDIR}
RAW=$OUT/raw
SIL="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
mkdir -p "$RAW/silesia" "$OUT/real"
: > "$OUT/MANIFEST.txt"

get() {
	curl -fsSL --retry 3 --retry-delay 5 --connect-timeout 30 --max-time 600 -o "$2" "$1"
}

get_silesia() {
	if get https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip "$RAW/silesia.zip" &&
	   unzip -q -o "$RAW/silesia.zip" -d "$RAW/silesia"; then
		echo "corpus silesia: sun.aei.polsl.pl silesia.zip" >> "$OUT/MANIFEST.txt"
		return 0
	fi
	local f
	for f in $SIL; do
		get "https://github.com/MiloszKrajewski/SilesiaCorpus/raw/master/$f.zip" "$RAW/$f.zip" &&
			unzip -q -o "$RAW/$f.zip" -d "$RAW/silesia" || return 1
	done
	echo "corpus silesia: github.com/MiloszKrajewski/SilesiaCorpus" >> "$OUT/MANIFEST.txt"
}

get_enwik8() {
	local u
	for u in https://mattmahoney.net/dc/enwik8.zip https://data.deepai.org/enwik8.zip; do
		if get "$u" "$RAW/enwik8.zip" && unzip -q -o "$RAW/enwik8.zip" -d "$RAW"; then
			echo "corpus enwik8: $u" >> "$OUT/MANIFEST.txt"
			return 0
		fi
	done
	return 1
}

get_silesia
get_enwik8
for f in $SIL; do
	python3 "$HERE/slice.py" "$RAW/silesia/$f" "$OUT/real/$f"
done
python3 "$HERE/slice.py" "$RAW/enwik8" "$OUT/real/enwik8"
if (cd "$OUT/real" && sha256sum -c --quiet "$HERE/slices.sha256"); then
	echo "real: 13 slices, SHA-256 identical to the local slices" >> "$OUT/MANIFEST.txt"
else
	echo "real: SLICES DIFFER from the local slices (slices.sha256)" >> "$OUT/MANIFEST.txt"
fi
rm -rf "$RAW"
(cd "$OUT" && sha256sum real/* >> MANIFEST.txt && ls -l real)
cat "$OUT/MANIFEST.txt"
