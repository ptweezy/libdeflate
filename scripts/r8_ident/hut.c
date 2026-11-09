/*
 * r6f4v hut.c: direct differential unit test of deflate_make_huffman_code()
 * and deflate_compute_precode_items().  Compiled once per tree by including
 * that tree's lib/deflate_compress.c; prints per-trial hashes of the lens of
 * all symbols, the codewords of the used symbols, and the precode items and
 * frequencies.  Outputs of two trees are compared with cmp.
 * usage: hut SEED NTRIALS
 *
 * Adapted for CI from handoff/tools/identity/hut.c: the cumulative hash is
 * printed every 1000 trials, so that a difference can be located, and the
 * order of two random numbers no longer depends on the compiler.
 */
#include "lib/deflate_compress.c"
#include <stdio.h>
#include <stdlib.h>

static u64 hs;
static u64 hr(void) { hs ^= hs << 13; hs ^= hs >> 7; hs ^= hs << 17; return hs; }
static u32 hn(u32 n) { return n ? (u32)(hr() % n) : 0; }
static u64 fnv(u64 h, u32 v) { h ^= v; return h * 0x100000001b3ULL; }

int main(int argc, char **argv)
{
	unsigned long seed = strtoul(argv[1], 0, 0), n = strtoul(argv[2], 0, 0), t;
	static u32 freqs[DEFLATE_MAX_NUM_SYMS], cw[DEFLATE_MAX_NUM_SYMS];
	static u8 lens[DEFLATE_MAX_NUM_SYMS + 64];
	static u8 plens[DEFLATE_NUM_LITLEN_SYMS + DEFLATE_NUM_OFFSET_SYMS + 8];
	static u32 pfreqs[DEFLATE_NUM_PRECODE_SYMS];
	static unsigned pitems[DEFLATE_NUM_LITLEN_SYMS + DEFLATE_NUM_OFFSET_SYMS];
	u64 all = 0xcbf29ce484222325ULL;

	hs = seed * 0x9E3779B97F4A7C15ULL + 12345;
	for (t = 0; t < n; t++) {
		static const unsigned ns[] = { DEFLATE_NUM_PRECODE_SYMS,
			DEFLATE_NUM_OFFSET_SYMS, DEFLATE_NUM_LITLEN_SYMS };
		static const unsigned ml[] = { MAX_PRE_CODEWORD_LEN,
			MAX_OFFSET_CODEWORD_LEN, MAX_LITLEN_CODEWORD_LEN };
		unsigned k = hn(3), num_syms = ns[k], used, i, s;
		unsigned mode = hn(8);
		u32 total;
		u64 h = 0xcbf29ce484222325ULL;
		unsigned ni;

		/* number of used symbols: biased to edges and the threshold */
		switch (hn(6)) {
		case 0: used = hn(4); break;
		case 1: used = num_syms * 3 / 4 - 2 + hn(5); break;
		case 2: used = num_syms - hn(3); break;
		default: used = hn(num_syms + 1); break;
		}
		if (used > num_syms)
			used = num_syms;
		memset(freqs, 0, sizeof(freqs));
		total = 0;
		for (i = 0; i < used; i++) {
			u32 f;
			do { s = hn(num_syms); } while (freqs[s]);
			switch (mode) {
			case 0: f = 1 + hn(3); break;			/* tiny */
			case 1: f = 1 + hn(num_syms + 4); break;	/* near counters edge */
			case 2: f = num_syms - 2 + hn(4); break;	/* last buckets */
			case 3: { u32 a = 1, b = 1, j, c;		/* Fibonacci: deep tree */
				for (j = 0; j < i % 26; j++) { c = a + b; a = b; b = c; }
				f = a; break; }
			case 4: f = 1 + hn(300000); break;		/* huge */
			case 5: f = 7; break;				/* all equal */
			case 6: f = 1u << hn(18); break;		/* powers of 2 */
			default: {	/* two statements: a fixed order of the calls */
				u32 m = 1 + hn(1000);
				f = 1 + (u32)(hr() % m); break; }
			}
			/* stay within the precondition: total <= MAX_BLOCK_LENGTH */
			if (total + f > 300000)
				f = 1 + hn(3);
			total += f;
			freqs[s] = f;
		}
		/* garbage in the outputs to catch reliance on old contents */
		for (i = 0; i < num_syms; i++) {
			lens[i] = (u8)hr();
			cw[i] = (u32)hr();
		}
		deflate_make_huffman_code(num_syms, ml[k], freqs, lens, cw);
		for (i = 0; i < num_syms; i++) {
			h = fnv(h, lens[i]);
			if (lens[i])
				h = fnv(h, cw[i]);
		}
		/* precode items on a random lens sequence with runs */
		{
			unsigned nl = 257 + hn(30) + 1 + hn(30), p = 0;
			while (p < nl) {
				unsigned r = hn(4) ? 1 + hn(3) : 1 + hn(160);
				u8 v = hn(3) ? (u8)hn(16) : 0;
				while (r-- && p < nl)
					plens[p++] = v;
			}
			for (i = 0; i < 8; i++)
				plens[nl + i] = (u8)hr();	/* bytes past the end */
			memset(pitems, 0xA5, sizeof(pitems));
			ni = deflate_compute_precode_items(plens, nl, pfreqs, pitems);
			h = fnv(h, ni);
			for (i = 0; i < ni; i++)
				h = fnv(h, pitems[i]);
			for (i = 0; i < DEFLATE_NUM_PRECODE_SYMS; i++)
				h = fnv(h, pfreqs[i]);
		}
		all = fnv(all, (u32)h ^ (u32)(h >> 32));
		if (getenv("HUT_ALL"))
			printf("T %lu k%u used%u mode%u h %016llx\n", t, k, used, mode, (unsigned long long)h);
		if (t % 1000 == 999 || t == n - 1)
			printf("%lu %016llx\n", t, (unsigned long long)all);
	}
	return 0;
}
