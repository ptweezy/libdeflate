#!/usr/bin/env python3
"""Make the input set for the identity tools in OUTDIR, and OUTDIR/LIST,
OUTDIR/FLVLIST and OUTDIR/MANIFEST.

usage: mkin.py OUTDIR [CORPUS_DIR]

Files (names use only [a-z0-9-], so that their order is the same everywhere):
  s-*      256 KiB prefixes of the 12 Silesia files.  They are downloaded
           (two sources are tried); if that fails, s-text, s-srctar and
           s-binary are made from files of this machine instead.  If
           CORPUS_DIR is given, CORPUS_DIR/silesia/* and CORPUS_DIR/enwik8
           are used and nothing is downloaded.
  enwik8   256 KiB prefix of enwik8 (left out if it can't be downloaded)
  y-*      the synthetic files of handoff/tools/identity/mkin.py (same code
           and seed): random, zeros, periods, small alphabets, runs, dense
  g-*      small alphabets as in handoff/tools/identity/gensyn.py (64 KiB)
  a-rev140 as in handoff/tools/identity/gen_adv.py (128 KiB)
  t-*      new: segments whose number of used litlen (t-lit*) or offset
           (t-off) symbols is around half of the alphabet, which is where
           the round-8 Huffman code changes switch between their two paths

LIST has all the names in the order in which the tools get them.  FLVLIST
has the names for flvtest: a text file first and a binary file last.
MANIFEST has the size and SHA-256 of every file, and where the corpus files
came from.
"""
import hashlib
import io
import os
import random
import struct
import subprocess
import sys
import urllib.request
import zipfile
import zlib

out = sys.argv[1]
corpus = sys.argv[2] if len(sys.argv) > 2 else None
os.makedirs(out, exist_ok=True)
PREFIX = 256 * 1024
SILESIA = ['dickens', 'mozilla', 'mr', 'nci', 'ooffice', 'osdb', 'reymont',
           'samba', 'sao', 'webster', 'x-ray', 'xml']
# SHA-256 of the first 256 KiB of each Silesia file and of enwik8 (only used
# to report whether the downloaded files are the usual ones).
KNOWN = {
    's-dickens': 'becc366f2948cfbaefb219df19f54b101a842737428b01c7538b871a35c3edf4',
    's-mozilla': '1baf584ca87856e51fada5fa257864019123d57aa8b9793c097eea145ee165a6',
    's-mr': '288236c90013320462fbe5864774054b26334be4ee59a8bde8a9797fbc9148f7',
    's-nci': '6f129b33bdf149bcdab85d1402a4f6aa4e81c0733f5fe76bb6f82bead260759e',
    's-ooffice': '41a8f0abe06c5c9e88665870be089e57c9b065bb3c27ac838cca48bcb793cb19',
    's-osdb': 'dfafdca22a0cc7b551554126b88105a632275c0251164b07ca47633e97d91a11',
    's-reymont': '63949f06bead1d6e40964427389b88df91aaf71e8f908e2e40fb968db3d8f682',
    's-samba': '21c5e99f13f8cac5e099fa4c8898831d3929b068e35cae2f1a4fa424f9804d7b',
    's-sao': '2e897282e031856af823048d977bddaf54869d43dea72ba544bec003d353db3a',
    's-webster': '357ddb5945737ae814b8e6f5eb83fb1abe2d1a4cb9d76f4104a3e258f19f2208',
    's-x-ray': '51c0a846c6cbe121f3f734d32100d60fd8bdd5b79ea9506d72608665e5fef7b1',
    's-xml': 'f2bbde07c2061c09fa41e4d4998c07847fbcc23d4ab166ca105a7f66ac8cd37b',
    'enwik8': '0cdfd008d5327addbf5b118b4a8d616a8cc2971627727b10bfb20e97920881e7',
}
names = []
notes = []


def put(name, data):
    assert name not in names, name
    assert all(c in 'abcdefghijklmnopqrstuvwxyz0123456789-' for c in name)
    assert len(data) > 0, name
    with open(os.path.join(out, name), 'wb') as f:
        f.write(data)
    names.append(name)


def fetch(url, timeout=300, rng=None):
    req = urllib.request.Request(url, headers={'User-Agent': 'curl/8'})
    if rng:
        req.add_header('Range', 'bytes=%d-%d' % rng)
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read()
        except Exception as e:  # noqa
            err = e
    raise err


def silesia_zip():
    z = zipfile.ZipFile(io.BytesIO(fetch(
        'https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip')))
    return {n: z.open(n).read(PREFIX) for n in SILESIA}


def silesia_mirror():
    d = {}
    for n in SILESIA:
        z = zipfile.ZipFile(io.BytesIO(fetch(
            'https://raw.githubusercontent.com/MiloszKrajewski/'
            'SilesiaCorpus/master/%s.zip' % n)))
        d[n] = z.open(n).read(PREFIX)
    return d


def enwik8_prefix():
    # The first part of the zip is enough: a local file header, then the
    # raw DEFLATE stream.
    for rng in ((0, 1024 * 1024 - 1), None):
        try:
            b = fetch('https://mattmahoney.net/dc/enwik8.zip', rng=rng)
            sig, _, _, method, _, _, _, _, _, nlen, xlen = struct.unpack(
                '<IHHHHHIIIHH', b[:30])
            assert sig == 0x04034b50 and method == 8
            d = zlib.decompressobj(-15).decompress(b[30 + nlen + xlen:],
                                                   PREFIX)
            if len(d) == PREFIX:
                return d
        except Exception as e:  # noqa
            notes.append('enwik8 download failed (%s): %r' % (rng, e))
    return None


def local_fallback():
    def cat(cmd, limit):
        return subprocess.run(cmd, shell=True, stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL).stdout[:limit]
    top = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    return {
        'text': cat("find /usr/include /usr/share/doc -type f -size -2M "
                    "2>/dev/null | LC_ALL=C sort | head -2000 | xargs cat",
                    PREFIX),
        'srctar': cat("tar -cf - -C '%s' lib programs" % top, PREFIX),
        'binary': cat("find /usr/lib /usr/bin -type f -size +100k "
                      "2>/dev/null | LC_ALL=C sort | head -40 | xargs cat",
                      PREFIX),
    }


if corpus:
    sil = {}
    for n in SILESIA:
        with open(os.path.join(corpus, 'silesia', n), 'rb') as f:
            sil[n] = f.read(PREFIX)
    notes.append('silesia: from ' + corpus)
    try:
        with open(os.path.join(corpus, 'enwik8'), 'rb') as f:
            enwik8 = f.read(PREFIX)
    except OSError:
        enwik8 = None
else:
    sil = None
    for src in (silesia_zip, silesia_mirror):
        try:
            sil = src()
            notes.append('silesia: downloaded by %s()' % src.__name__)
            break
        except Exception as e:  # noqa
            notes.append('silesia: %s() failed: %r' % (src.__name__, e))
    if sil is None:
        sil = local_fallback()
        notes.append('silesia: NOT AVAILABLE; s-text, s-srctar and s-binary '
                     'were made from files of this machine')
    enwik8 = enwik8_prefix()
for n in sorted(sil):
    put('s-' + n, sil[n])
if enwik8:
    put('enwik8', enwik8)
else:
    notes.append('enwik8: left out')

# --- the synthetic files of handoff/tools/identity/mkin.py, unchanged ---
r = random.Random(20261108)
N = 128 * 1024


def from_alphabet(alpha, n=N, weights=None):
    return bytes(r.choices(alpha, weights=weights, k=n))


put('y-random', bytes(r.getrandbits(8) for _ in range(N)))
put('y-zeros', bytes(64 * 1024))
put('y-period3', (b'abc' * (N // 3 + 1))[:N])
put('y-period7-noise', bytes((b'0123456'[i % 7] if r.random() > 0.01 else
                              r.getrandbits(8)) for i in range(N)))
put('y-hex', from_alphabet(b'0123456789abcdef'))
put('y-base64', from_alphabet(
    b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'))
put('y-digits', from_alphabet(b'0123456789'))
put('y-dna', from_alphabet(b'ACGT'))
put('y-binary2', from_alphabet(b'\x00\xff'))
put('y-lower', from_alphabet(b'abcdefghijklmnopqrstuvwxyz '))
put('y-skew16', from_alphabet(bytes(range(16)),
                              weights=[2 ** -i for i in range(16)]))
# Runs of random lengths and values: exercises long zero/nonzero length runs.
runs = bytearray()
while len(runs) < N:
    runs += bytes([r.getrandbits(8)]) * r.choice([1, 2, 3, 4, 7, 11, 138, 300])
put('y-runs', bytes(runs[:N]))
# All 256 byte values, each frequency distinct (dense, deep Huffman trees).
dense = bytearray()
for v in range(256):
    dense += bytes([v]) * (1 + v * 3)
r.shuffle(dense)
put('y-dense-fib', bytes(dense))

# --- as in gensyn.py, but 64 KiB each ---
r = random.Random(5031108)
G = 64 * 1024
put('g-abc', bytes(r.choice(b'abc') for _ in range(G)))
put('g-skew2', bytes(r.choices(b'01', weights=[95, 5], k=G)))
put('g-half', bytes(r.randrange(128) for _ in range(G)))
b = bytearray()
while len(b) < G:
    b += (','.join(str(r.randrange(10**6, 10**7)) for _ in range(6)) +
          '\n').encode()
put('g-csv', bytes(b[:G]))
b = bytearray(G)
i = 0
while True:
    i += 1 + int(r.expovariate(1 / 40))
    if i >= G:
        break
    b[i] = r.randrange(1, 256)
put('g-sparse', bytes(b))

# --- as rev140 in gen_adv.py, but 128 KiB ---
r = random.Random(20261109)
S16 = set(i * 288 // 16 for i in range(16))
non = [v for v in range(256) if v not in S16]
put('a-rev140', bytes(r.choices(
    non[:140], weights=[2.2 - 1.2 * i / 139 for i in range(140)], k=N)))

# --- new: blocks around the "at most half of the symbols are used" switch ---
r = random.Random(8020261109)


def lit_segment(n, k, p, skew):
    """n bytes using exactly k byte values (all of them in the first k
    bytes), with a copy of earlier data started with probability p."""
    alpha = r.sample(range(256), k)
    wts = [1.0 / (1 + i) ** 0.7 for i in range(k)] if skew else None
    buf = bytearray(alpha[:n])
    while len(buf) < n:
        if p and r.random() < p:
            ln = r.randint(3, 40) if r.random() < .9 else r.randint(3, 258)
            d = r.randint(1, len(buf))
            for _ in range(min(ln, n - len(buf))):
                buf.append(buf[-d])
        else:
            buf.append(r.choices(alpha, weights=wts)[0] if wts
                       else r.choice(alpha))
    return bytes(buf)


def lit_file(total, seglen, ks, ps):
    data = bytearray()
    while len(data) < total:
        for k in ks:
            for p in ps:
                if len(data) < total:
                    data += lit_segment(seglen, k, p, r.random() < .5)
    return bytes(data[:total])


# The litlen alphabet has 288 symbols; a block with k byte values and no
# matches uses k + 1 of them, and matches add length symbols.
put('t-lit333', lit_file(64 * 1024, 333, range(130, 157), (0, .02)))
put('t-lit1k', lit_file(64 * 1024, 1024, range(134, 153), (0, .01, .05)))
put('t-lit4k', lit_file(128 * 1024, 4096, range(118, 150, 2), (.02, .1, .3)))


def off_segment(n, maxslot):
    """n bytes over 64 byte values, about a third of them copied from an
    offset in a random one of the first maxslot offset slots."""
    alpha = r.sample(range(256), 64)
    buf = bytearray(r.choice(alpha) for _ in range(8))
    while len(buf) < n:
        if r.random() < .06:
            slot = r.randrange(maxslot)
            if slot < 4:
                d = slot + 1
            else:
                nbits = (slot - 2) // 2
                base = (2 + (slot & 1)) << nbits
                d = base + 1 + r.randrange(1 << nbits)
            d = min(d, len(buf))
            for _ in range(min(r.randint(3, 12), n - len(buf))):
                buf.append(buf[-d])
        else:
            buf.append(r.choice(alpha))
    return bytes(buf)


# The offset alphabet has 32 symbols, so the switch is at 16 used slots.
data = bytearray()
while len(data) < 64 * 1024:
    for maxslot in range(11, 23):
        for seglen in (1024, 2048, 4096):
            data += off_segment(seglen, maxslot)
put('t-off', bytes(data[:64 * 1024]))

# --- lists and manifest ---
names.sort()
with open(os.path.join(out, 'LIST'), 'w', newline='\n') as f:
    f.write(''.join(n + '\n' for n in names))
s_names = [n for n in names if n.startswith('s-')]
first = 's-dickens' if 's-dickens' in s_names else 's-text'
last = 's-mozilla' if 's-mozilla' in s_names else 's-binary'
flv = [first] + [n for n in s_names if n not in (first, last)] + [last]
with open(os.path.join(out, 'FLVLIST'), 'w', newline='\n') as f:
    f.write(''.join(n + '\n' for n in flv))
ok = 0
with open(os.path.join(out, 'MANIFEST'), 'w', newline='\n') as f:
    for n in names:
        with open(os.path.join(out, n), 'rb') as g:
            d = g.read()
        h = hashlib.sha256(d).hexdigest()
        tag = ''
        if n in KNOWN:
            tag = ' known' if KNOWN[n] == h else ' UNEXPECTED'
            ok += KNOWN[n] == h
        f.write('%s %7d %s%s\n' % (h, len(d), n, tag))
    f.write('# files: %d, bytes: %d\n' % (
        len(names), sum(os.path.getsize(os.path.join(out, n)) for n in names)))
    f.write('# corpus prefixes with the expected SHA-256: %d of %d\n' % (
        ok, len(KNOWN)))
    f.write('# python %s\n' % sys.version.split()[0])
    for n in notes:
        f.write('# ' + n + '\n')
print(open(os.path.join(out, 'MANIFEST')).read(), end='')
