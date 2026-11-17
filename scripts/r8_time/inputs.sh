#!/bin/bash
# r8_time inputs.sh - benchmark inputs for the timing shards (test-only).
# usage: inputs.sh OUTDIR
#
# OUTDIR/real  13 files of 2 MiB: slice.py's slice of each Silesia file and
#              of enwik8 (bit-identical to the local slices if the
#              downloads are the usual files; checked against slices.sha256)
# OUTDIR/adv   gen_adv.py's dense alphabets (avoid241 avoid160 s5mix rev140
#              rnd256, 2 MiB each) and gen_thr.py's alphabets around the
#              2*used = num_syms threshold (thr1k thr4k thr64k)
# OUTDIR/syn   gensyn.py's small alphabets (abc skew2 half rnd256 csv sparse)
# OUTDIR/MANIFEST.txt  where the corpus came from, and every file's SHA-256
#
# Corpus sources, in order: the official silesia.zip, then a GitHub mirror
# of the same files; enwik8.zip from mattmahoney.net, then a mirror.  If
# all fail, "real" is built from files of this machine (text, binaries, the
# source tree) and MANIFEST.txt says "corpus fallback": such a run measures
# something, but not the usual corpus.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
OUT=${1:?usage: inputs.sh OUTDIR}
RAW=$OUT/raw
SIL="dickens mozilla mr nci ooffice osdb reymont samba sao webster x-ray xml"
mkdir -p "$RAW/silesia" "$OUT/real" "$OUT/adv" "$OUT/syn"
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

fallback() {
	echo "corpus fallback: downloads failed; files of this machine" >> "$OUT/MANIFEST.txt"
	rm -rf "$OUT/real"; mkdir -p "$OUT/real" "$RAW/fb"
	find /usr/include /usr/share/doc -type f -size -2M 2>/dev/null | sort | head -20000 |
		xargs cat 2>/dev/null | head -c 30000000 > "$RAW/fb/text" || true
	find /usr/lib /usr/bin -type f -size +100k 2>/dev/null | sort | head -400 |
		xargs cat 2>/dev/null | head -c 30000000 > "$RAW/fb/binary" || true
	tar -cf - -C "$TOP" lib programs 2>/dev/null | head -c 10000000 > "$RAW/fb/srctar" || true
	local f
	for f in text binary srctar; do
		python3 "$HERE/slice.py" "$RAW/fb/$f" "$OUT/real/$f"
	done
}

if get_silesia && get_enwik8; then
	for f in $SIL; do
		python3 "$HERE/slice.py" "$RAW/silesia/$f" "$OUT/real/$f"
	done
	python3 "$HERE/slice.py" "$RAW/enwik8" "$OUT/real/enwik8"
	if (cd "$OUT/real" && sha256sum -c --quiet "$HERE/slices.sha256"); then
		echo "real: 13 slices, SHA-256 identical to the local slices" >> "$OUT/MANIFEST.txt"
	else
		echo "real: SLICES DIFFER from the local slices (slices.sha256)" >> "$OUT/MANIFEST.txt"
	fi
else
	fallback
fi
rm -rf "$RAW"

python3 "$HERE/gen_adv.py" "$OUT/adv"
python3 "$HERE/gen_thr.py" "$OUT/adv"
python3 "$HERE/gensyn.py" "$OUT/syn"
mkdir -p "$OUT/z3"
python3 "$HERE/gen_z3.py" "$OUT/z3"
cp "$OUT/adv/rev140" "$OUT/adv/avoid160" "$OUT/adv/thr1k" "$OUT/z3/"
for f in dickens mozilla; do [ ! -f "$OUT/real/$f" ] || cp "$OUT/real/$f" "$OUT/z3/"; done

(cd "$OUT" && sha256sum real/* adv/* syn/* z3/* >> MANIFEST.txt && ls -l real adv syn z3)
cat "$OUT/MANIFEST.txt"
