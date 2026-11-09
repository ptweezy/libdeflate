/*
 * utd.c: unit test of deflate_make_huffman_code() and
 * deflate_compute_precode_items(), in digest form.  Compiled once per tree by
 * including that tree's lib/deflate_compress.c; the outputs of two trees are
 * compared with cmp.
 *
 * This runs the test cases of handoff/tools/identity/ut/ut.c, which links the
 * functions of two trees into one program with objcopy and compares their
 * results directly.  That needs binutils for the target, so here each tree
 * instead prints a cumulative hash of its results every 1000 cases:
 *  - deflate_compute_precode_items(): every sequence over {0,1,15} up to
 *    length 12, over {0,5} up to length 18 and over {0,3,4,8} up to length 9,
 *    then random sequences with runs.  The number of items, the items and the
 *    precode frequencies are hashed.
 *  - deflate_make_huffman_code(): alphabets of 288, 32, 19 or 3..288 symbols,
 *    with the number of used symbols at, just above and just below half of
 *    them, 0..3, all, or random; several choices of which symbols are used
 *    and of the frequencies.  The lengths of all symbols and the codewords of
 *    the used symbols are hashed.  The outputs are prefilled with garbage.
 *
 * usage: utd HUFF_TRIALS RAND_PRECODE_TRIALS [SEED]
 */
#include "lib/deflate_compress.c"
#include <stdio.h>
#include <stdlib.h>

static u64 rs = 88172645463325252ULL;
static u64 rnd(void)
{
	rs ^= rs << 13;
	rs ^= rs >> 7;
	rs ^= rs << 17;
	return rs;
}
static unsigned ru(unsigned n) { return n ? (unsigned)(rnd() % n) : 0; }

static unsigned ctz64(u64 v)
{
	unsigned n = 0;

	while (!(v & 1)) {
		v >>= 1;
		n++;
	}
	return n;
}

static u64 fnv(u64 h, u32 v)
{
	h ^= v;
	return h * 0x100000001b3ULL;
}

static u64 all = 0xcbf29ce484222325ULL;
static unsigned long nflag, nhuff, ncpi, nused_hist[3], ncase;
static const char *phase = "";

static void flag(const char *what)
{
	if (nflag++ < 20)
		printf("FLAG: %s\n", what);
}

static void end_case(u64 h)
{
	all = fnv(all, (u32)h ^ (u32)(h >> 32));
	if (++ncase % 1000 == 0)
		printf("%s %lu %016llx\n", phase, ncase, (unsigned long long)all);
}

static void end_phase(void)
{
	printf("%s end %lu %016llx\n", phase, ncase, (unsigned long long)all);
	ncase = 0;
}

static void check_huff(unsigned num_syms, unsigned max_len, const u32 *fin)
{
	u32 *freqs = malloc(num_syms * sizeof(u32));
	u8 *lens = malloc(num_syms);
	u32 *cw = malloc(num_syms * sizeof(u32));
	unsigned i, used = 0;
	unsigned g = ru(3);
	u64 h = 0xcbf29ce484222325ULL;

	memcpy(freqs, fin, num_syms * sizeof(u32));
	for (i = 0; i < num_syms; i++) {
		lens[i] = g == 0 ? 0 : (u8)rnd();
		cw[i] = g == 0 ? 0 : g == 1 ? 0xFFFFFFFF : (u32)rnd();
		used += freqs[i] != 0;
	}
	nused_hist[2 * used <= num_syms ? (2 * used == num_syms ? 1 : 0) : 2]++;
	deflate_make_huffman_code(num_syms, max_len, freqs, lens, cw);
	nhuff++;
	if (memcmp(freqs, fin, num_syms * sizeof(u32))) {
		flag("freqs modified");
		h = fnv(h, 0xF00D);
	}
	h = fnv(h, num_syms);
	h = fnv(h, max_len);
	for (i = 0; i < num_syms; i++) {
		h = fnv(h, lens[i]);
		if (lens[i] != 0)
			h = fnv(h, cw[i]);
		if (lens[i] > max_len) {
			flag("len > max_len");
			h = fnv(h, 0xBAD);
		}
	}
	free(freqs);
	free(lens);
	free(cw);
	end_case(h);
}

#define FREQ_LIMIT ((1u << 22) - 1)

static void gen_huff(void)
{
	static u32 f[288];
	static unsigned perm[288];
	unsigned num_syms, max_len, num_used, i, minlen;
	unsigned long long sum;
	unsigned cap;
	unsigned pm, fm;

	switch (ru(5)) {
	case 0: num_syms = 288; max_len = ru(2) ? 14 : 15; break;
	case 1: num_syms = 32; max_len = 15; break;
	case 2: num_syms = 19; max_len = 7; break;
	case 3: num_syms = 288; max_len = 9 + ru(7); break;
	default:
		num_syms = 3 + ru(286);	/* master needs >= 3 counters */
		for (minlen = 1; (1u << minlen) < num_syms; minlen++)
			;
		max_len = minlen + ru(16 - minlen);
		break;
	}
	switch (ru(8)) {
	case 0: num_used = num_syms / 2; break;
	case 1: num_used = num_syms / 2 + 1; break;
	case 2: num_used = (num_syms + 1) / 2; break;
	case 3: num_used = ru(4); break;
	case 4: num_used = num_syms; break;
	case 5: num_used = num_syms / 2 >= 3 ? num_syms / 2 - ru(3) : 0; break;
	default: num_used = ru(num_syms + 1); break;
	}
	if (num_used > num_syms)
		num_used = num_syms;
	for (i = 0; i < num_syms; i++)
		perm[i] = i;
	pm = ru(5);
	if (pm == 0) {		/* random subset */
		for (i = 0; i < num_syms; i++) {
			unsigned j = i + ru(num_syms - i), t = perm[i];
			perm[i] = perm[j];
			perm[j] = t;
		}
	} else if (pm == 1) {	/* the top num_used symbols */
		for (i = 0; i < num_syms; i++)
			perm[i] = num_syms - 1 - i;
	} else if (pm == 2) {	/* every other one from the top, then the rest */
		unsigned k = 0;
		for (i = num_syms; i-- > 0; )
			if ((num_syms - 1 - i) % 2 == 0)
				perm[k++] = i;
		for (i = num_syms; i-- > 0; )
			if ((num_syms - 1 - i) % 2 == 1)
				perm[k++] = i;
	}			/* else the bottom ones */
	memset(f, 0, sizeof(f));
	cap = num_used ? FREQ_LIMIT / num_used : 1;
	fm = ru(8);
	{
		unsigned fa = 1, fb = 1;
		for (i = 0; i < num_used; i++) {
			u32 v;

			switch (fm) {
			case 0: v = 1; break;
			case 1: v = 1 + ru(5); break;
			case 2: v = 1 + ru(1u << 20); break;
			case 3: /* Fibonacci: deep trees, length limiting */
				v = fa;
				if (fb < 400000) {
					unsigned t = fa + fb;
					fa = fb;
					fb = t;
				}
				break;
			case 4: v = num_syms - 1 + ru(3); break;	/* last counter */
			case 5: v = ru(2) ? 1 : (1u << 21) / (num_used + 1) + 1; break;
			case 6: v = 1u << ru(21); break;
			default: v = num_syms - 2 + ru(3); break;	/* around the last counter */
			}
			if (v > cap)
				v = cap;
			if (v == 0)
				v = 1;
			f[perm[i]] = v;
		}
	}
	for (sum = 0, i = 0; i < num_syms; i++)
		sum += f[i];
	if (sum > FREQ_LIMIT)
		return;
	check_huff(num_syms, max_len, f);
}

static void check_cpi(const u8 *lin, unsigned n)
{
	u8 *l = malloc(n);
	unsigned *items = malloc(n * sizeof(unsigned));
	u32 freqs[DEFLATE_NUM_PRECODE_SYMS];
	unsigned ni, i;
	u64 h = 0xcbf29ce484222325ULL;

	memcpy(l, lin, n);
	memset(freqs, 0xA5, sizeof(freqs));
	ni = deflate_compute_precode_items(l, n, freqs, items);
	ncpi++;
	h = fnv(h, n);
	h = fnv(h, ni);
	if (ni > n) {
		flag("too many items");
		ni = n;
	}
	for (i = 0; i < ni; i++)
		h = fnv(h, items[i]);
	for (i = 0; i < DEFLATE_NUM_PRECODE_SYMS; i++)
		h = fnv(h, freqs[i]);
	if (memcmp(l, lin, n)) {
		flag("lens modified");
		h = fnv(h, 0xF00D);
	}
	free(l);
	free(items);
	end_case(h);
}

static void exhaustive(const u8 *alpha, unsigned asz, unsigned maxn)
{
	u8 v[32];
	unsigned idx[32];
	unsigned n, i;

	for (n = 1; n <= maxn; n++) {
		memset(idx, 0, sizeof(idx));
		for (;;) {
			for (i = 0; i < n; i++)
				v[i] = alpha[idx[i]];
			check_cpi(v, n);
			for (i = 0; i < n; i++) {
				if (++idx[i] < asz)
					break;
				idx[i] = 0;
			}
			if (i == n)
				break;
		}
	}
}

static void rand_cpi(void)
{
	static u8 v[320];
	unsigned n = 1 + ru(320), i = 0;
	unsigned mode = ru(4);
	u8 pal[4];

	pal[0] = 0;
	pal[1] = (u8)(1 + ru(15));
	pal[2] = (u8)(1 + ru(15));
	pal[3] = (u8)(1 + ru(15));
	while (i < n) {
		unsigned run, j;
		u8 len;

		switch (mode) {
		case 0: run = 1 + ru(4); break;
		case 1: run = 1 + ru(12); break;
		case 2: run = ru(4) ? 1 + ru(6) : 1 + ru(200); break;
		default: run = 1 + ctz64(rnd() | (1ULL << 40)) * 3; break;
		}
		len = ru(3) ? pal[ru(4)] : (u8)ru(16);
		for (j = 0; j < run && i < n; j++)
			v[i++] = len;
	}
	check_cpi(v, n);
}

int main(int argc, char **argv)
{
	long th = argc > 1 ? atol(argv[1]) : 100000;
	long tp = argc > 2 ? atol(argv[2]) : 100000;
	long i;
	static const u8 a3[] = { 0, 1, 15 };
	static const u8 a2[] = { 0, 5 };
	static const u8 a4[] = { 0, 3, 4, 8 };

	if (argc > 3)
		rs ^= strtoull(argv[3], NULL, 0) * 0x9E3779B97F4A7C15ULL;
	phase = "cpi-0-1-15";
	exhaustive(a3, 3, 12);
	end_phase();
	phase = "cpi-0-5";
	exhaustive(a2, 2, 18);
	end_phase();
	phase = "cpi-0-3-4-8";
	exhaustive(a4, 4, 9);
	end_phase();
	phase = "cpi-random";
	for (i = 0; i < tp; i++)
		rand_cpi();
	end_phase();
	phase = "huffman";
	for (i = 0; i < th; i++)
		gen_huff();
	end_phase();
	printf("huffman trials %lu (used<half %lu, ==half %lu, >half %lu), precode trials %lu, flags %lu\n",
	       nhuff, nused_hist[0], nused_hist[1], nused_hist[2], ncpi, nflag);
	return nflag != 0;
}
