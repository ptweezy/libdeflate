/*
 * decompress_multilane.h - multi-lane speculative decoding of Huffman blocks
 *
 * Included by decompress_template.h when MLSD_ENABLED (see
 * deflate_decompress.c).
 *
 * The fastloop is limited by the serial chain of table lookups and by
 * mispredicted branches on the symbol types.  But Huffman codes
 * self-synchronize: decoding from an arbitrary bit position soon (after about
 * 100 bits on real data) lands on a true symbol boundary, and from then on
 * yields the true symbols.  So MLSD_LANES "lanes" decode parts of a block at
 * once, without data-dependent branches: lane 0 from the known position, lane
 * i from i times the distance that a lane covers in a round, less a margin.
 * Each step of a lane decodes a litlen entry (and the offset that follows a
 * length) into a 64-bit record, and saves its bit position.
 *
 * Lane i+1 takes over where lane i ended if that's one of its positions (found
 * by binary search): decoding is deterministic, so from there on both decode
 * the same symbols.  If it isn't, lane i is extended serially until it is.
 * The verified records are executed while the lanes decode the next round:
 * the lanes are bound by ALU throughput, the execution by stores.
 *
 * The lanes use their own decode tables, built from the codeword lengths, with
 * 64-bit entries that hold the record bits ready to be added up.  Litlen
 * entries (with compound entries for a literal followed by a literal or a
 * length whose codewords fit in the main table, like the normal table):
 *
 *	Bit 63:     1 if EOB, or if the match length can exceed MLSD_COPY
 *	Bit 58-32:  bits 26-0 of the record, or the subtable start index
 *	Bit 31-27:  extra length bits mask
 *	Bit 21-16:  codeword length (subtable bits, if subtable pointer)
 *	Bit 14:     1 if subtable pointer (MLSD_SUB)
 *	Bit 8:      1 if no offset follows (MLSD_NOOFF)
 *	Bit 5-0:    codeword length + extra bits (main table bits, if pointer)
 *
 * Offset entries are the same except: bit 63 = 1 if the offset can be < 16,
 * bits 47-32 = offset base, bits 31-19 = extra bits mask, and bits 13-8 =
 * codeword length.  Entries 256-511 of the offset table consume no bits and
 * give MLSD_DUMMY_OFF; steps that decode literals or EOB OR MLSD_NOOFF into
 * the offset table index, so they need no selects.  Records:
 *
 *	Bit 63:     1 if the offset can be < 16
 *	Bit 47-32:  match offset (MLSD_DUMMY_OFF if none)
 *	Bit 31:     1 if EOB, or if the match length can exceed MLSD_COPY
 *	Bit 26-18:  match length (0 if none, MLSD_EOB_LEN if EOB)
 *	Bit 17-16:  number of literals (0-2)
 *	Bit 15-0:   the literal(s)
 */

#ifndef MLSD_MIN_STEPS	/* (the tunables can be overridden for testing) */
#  define MLSD_MIN_STEPS	128	/* steps per lane per round at first */
#  define MLSD_GROW		4096	/* ... until this many records */
#endif
#define MLSD_COPY		64	/* bytes copied for every record */
#define MLSD_EOB_LEN		511
#define MLSD_DUMMY_OFF		4096
#define MLSD_MAX_STEP_BITS	(LENGTH_MAXBITS + OFFSET_MAXBITS)
#define MLSD_MAX_REC_OUT	(2 + DEFLATE_MAX_MATCH_LEN)
/*
 * Blocks use the lanes only if at least MLSD_MIN_IN_BYTES of input remain and
 * MLSD_MIN_OUT_BYTES of output space, the previous block produced at least
 * MLSD_MIN_PREV_OUT bytes, and the length codewords take at least 1/8 of the
 * codespace (the fastloop does better on runs of literals).
 */
#ifndef MLSD_MIN_PREV_OUT
#  define MLSD_MIN_IN_BYTES	4096
#  define MLSD_MIN_OUT_BYTES	32768
#  define MLSD_MIN_PREV_OUT	4096
#  define MLSD_MIN_LEN_SPACE	(1U << (DEFLATE_MAX_CODEWORD_LEN - 3))
#endif

#ifndef MLSD_SHARED_DEFINED
#define MLSD_SHARED_DEFINED
#define MLSD_SUB		((u64)1 << 14)
#define MLSD_NOOFF		((u64)1 << 8)
#define MLSD_SLOW		(((u64)1 << 63) | ((u64)1 << 31))
#if defined(__clang__)
#  define MLSD_UNROLL_LOOP	_Pragma("clang loop unroll(full)")
#else
#  define MLSD_UNROLL_LOOP	_Pragma("GCC unroll 8")
#endif

#include <sys/qos.h>
#include <sys/resource.h>
#ifndef PRIO_DARWIN_THREAD	/* (hidden by a strict _POSIX_C_SOURCE) */
#  define PRIO_DARWIN_THREAD	3
#endif
#ifndef PRIO_DARWIN_PROCESS
#  define PRIO_DARWIN_PROCESS	4
#endif

/*
 * Is this thread likely to run on the efficiency cores, where the lanes lose:
 * does it have a utility or lower QoS class, or is it or its process in the
 * background ("darwinbg") state?
 */
static bool
mlsd_low_priority(void)
{
	const qos_class_t q = qos_class_self();

#ifdef MLSD_TEST_ANY_PRIORITY
	return false;
#endif
	return (q != QOS_CLASS_UNSPECIFIED && q <= QOS_CLASS_UTILITY) ||
	       getpriority(PRIO_DARWIN_PROCESS, 0) > 0 ||
	       getpriority(PRIO_DARWIN_THREAD, 0) > 0;
}

/* The lane-format entry for symbol 'sym' with codeword length 'len' */
static forceinline u64
mlsd_entry(unsigned sym, unsigned len, bool litlen)
{
	u32 res, base, mask;

	if (litlen && sym < DEFLATE_NUM_LITERALS)
		return MLSD_NOOFF | ((u64)sym << 32) | ((u64)1 << 48) |
		       (len << 16) | len;
	if (litlen && sym == DEFLATE_END_OF_BLOCK)
		return MLSD_NOOFF | ((u64)1 << 63) |
		       ((u64)MLSD_EOB_LEN << 50) | (len << 16) | len;
	res = litlen ? litlen_decode_results[sym] : offset_decode_results[sym];
	base = res >> 16;
	mask = (1U << (res & 0x1F)) - 1;
	len += (len << (litlen ? 16 : 8)) + (res & 0x1F);
	if (litlen)
		return ((u64)(base + mask > MLSD_COPY) << 63) |
		       ((u64)base << 50) | ((u64)mask << 27) | len;
	return ((u64)(base < 16) << 63) | ((u64)base << 32) |
	       ((u64)mask << 19) | len;
}
#endif /* MLSD_SHARED_DEFINED */

/*
 * Build a lane-format decode table with 2^tb main entries, and subtables from
 * index 'sub_base', from the codeword lengths 'lens'.  Like
 * build_decode_table(), fill the main table one codeword length at a time,
 * doubling it in between, and add the compound entries (litlen code only) of
 * total length 'len' when it has 2^len entries.  Return false, and leave it to
 * build_decode_table(), unless the code is complete.
 */
static ATTRIBUTES bool
ADD_SUFFIX(mlsd_build_table)(u64 * restrict t, const u8 * restrict lens,
			     unsigned num_syms, unsigned tb, unsigned sub_base,
			     bool litlen, u16 * restrict sorted)
{
	u8 cnt4[4][DEFLATE_MAX_CODEWORD_LEN + 1] = { { 0 } };
	unsigned count[DEFLATE_MAX_CODEWORD_LEN + 1];
	unsigned start[DEFLATE_MAX_CODEWORD_LEN + 2];
	unsigned pos[DEFLATE_MAX_CODEWORD_LEN + 1];
	u16 rc[DEFLATE_MAX_NUM_SYMS];	/* codewords (bit-reversed), sorted */
	u64 ent[DEFLATE_MAX_NUM_SYMS];	/* entries, sorted */
	unsigned sym, len, i, n, cw = 0, end = sub_base, sub = 0, prefix = ~0U;
	u32 space = 0;

	/* (4 interleaved counts, as consecutive lengths are often equal) */
	for (sym = 0; sym + 4 <= num_syms; sym += 4) {
		cnt4[0][lens[sym]]++;
		cnt4[1][lens[sym + 1]]++;
		cnt4[2][lens[sym + 2]]++;
		cnt4[3][lens[sym + 3]]++;
	}
	for (; sym < num_syms; sym++)
		cnt4[0][lens[sym]]++;
	start[1] = 0;
	for (len = 1; len <= DEFLATE_MAX_CODEWORD_LEN; len++) {
		count[len] = cnt4[0][len] + cnt4[1][len] + cnt4[2][len] +
			     cnt4[3][len];
		space += count[len] << (DEFLATE_MAX_CODEWORD_LEN - len);
		pos[len] = start[len];
		start[len + 1] = start[len] + count[len];
	}
	if (space != 1U << DEFLATE_MAX_CODEWORD_LEN)
		return false;
	for (sym = 0; sym < num_syms; sym++) {
		if (lens[sym] != 0)
			sorted[pos[lens[sym]]++] = sym;
	}
	for (i = 0, len = 1; len <= tb; len++) {
		memcpy(&t[1U << (len - 1)], t, sizeof(t[0]) << (len - 1));
		for (n = count[len]; n != 0; n--, i++) {
			rc[i] = cw;
			ent[i] = t[cw] = mlsd_entry(sorted[i], len, litlen);
			if (cw != (1U << len) - 1) {	/* next codeword */
				sym = 1U << bsr32(cw ^ ((1U << len) - 1));
				cw = (cw & (sym - 1)) | sym;
			}
		}
		for (n = 1; litlen && n < len; n++) {
			unsigned a, b;

			/* 1st codeword: a literal of length n */
			if (start[n] == start[n + 1] ||
			    sorted[start[n]] >= DEFLATE_NUM_LITERALS)
				continue;
			for (b = start[len - n]; b < start[len - n + 1]; b++) {
				u64 * const dst = &t[(unsigned)rc[b] << n];
				const u64 lit2 = ent[b] & ((u64)0xFF << 32);
				const u64 e = ent[b] + (lit2 << 8) - lit2 +
					      ((u64)1 << 48) + n * 0x10001;

				if (sorted[b] == DEFLATE_END_OF_BLOCK)
					continue;
				for (a = start[n]; a < start[n + 1] &&
				     sorted[a] < DEFLATE_NUM_LITERALS; a++)
					dst[rc[a]] = e + ((u64)sorted[a] << 32);
			}
		}
	}
	/* Longer codewords, in subtables (as in build_decode_table()) */
	for (; i < start[DEFLATE_MAX_CODEWORD_LEN + 1]; i++) {
		u64 e;

		len = lens[sorted[i]];
		if ((cw & ((1U << tb) - 1)) != prefix) {
			unsigned bits = len - tb;
			u32 used = start[len + 1] - i;

			while (used < 1U << bits) {
				bits++;
				used = (used << 1) + count[tb + bits];
			}
			prefix = cw & ((1U << tb) - 1);
			sub = end;
			end += 1U << bits;
			t[prefix] = ((u64)sub << 32) | MLSD_SUB |
				    (bits << (litlen ? 16 : 8)) | tb;
		}
		e = mlsd_entry(sorted[i], len - tb, litlen);
		for (n = sub + (cw >> tb); n < end; n += 1U << (len - tb))
			t[n] = e;
		if (cw != (1U << len) - 1) {
			n = 1U << bsr32(cw ^ ((1U << len) - 1));
			cw = (cw & (n - 1)) | n;
		}
	}
	return true;
}

/*
 * Build the lanes' tables from d->u.l.lens (leaving them intact), or return
 * false if the lanes shouldn't or can't be used for this block.
 */
static NOINLINE ATTRIBUTES bool
ADD_SUFFIX(mlsd_build_tables)(struct libdeflate_decompressor * restrict d,
			      unsigned num_litlen_syms,
			      unsigned num_offset_syms)
{
	const u8 *lens = d->u.l.lens;
	u32 len_space = 0;
	unsigned i;

	/* (d->u.mlsd.lt[] has LITLEN_ENOUGH entries) */
	STATIC_ASSERT(MLSD_TABLEBITS == LITLEN_TABLEBITS);
	for (i = DEFLATE_FIRST_LEN_SYM; i < num_litlen_syms; i++) {
		if (lens[i] != 0)
			len_space += (1U << 15) >> lens[i];
	}
	if (len_space < MLSD_MIN_LEN_SPACE)
		return false;
	for (i = 1U << OFFSET_TABLEBITS; i < 2U << OFFSET_TABLEBITS; i++)
		d->u.mlsd.ot[i] = (u64)MLSD_DUMMY_OFF << 32;
	return ADD_SUFFIX(mlsd_build_table)(d->u.mlsd.ot,
					    lens + num_litlen_syms,
					    num_offset_syms, OFFSET_TABLEBITS,
					    2U << OFFSET_TABLEBITS, false,
					    d->sorted_syms) &&
	       ADD_SUFFIX(mlsd_build_table)(d->u.mlsd.lt, lens,
					    num_litlen_syms, MLSD_TABLEBITS,
					    1U << MLSD_TABLEBITS, true,
					    d->sorted_syms);
}

/* Resolve a subtable pointer into an entry that looks like a main-table one. */
static forceinline u64
ADD_SUFFIX(mlsd_sub)(const u64 *table, u64 entry, u64 bits, unsigned cw_shift)
{
	const u32 mb = entry & 63;
	const u64 sub = table[(u32)(entry >> 32) +
			      ((bits >> mb) &
			       ((1U << ((entry >> cw_shift) & 63)) - 1))];

	return sub + mb + ((u64)mb << cw_shift);
}

/*
 * Decode one litlen entry (and the offset, if a length) at bit position 'p'
 * into the record *rp, with no data-dependent branches except for the rare
 * subtable lookups.  Save the position in *pp and advance 'p'.
 */
#define MLSD_STEP(p, rp, pp)						\
do {									\
	const u64 w_ = get_unaligned_le64(in + ((p) >> 3)) >> ((p) & 7);\
	u64 e1_ = lt[w_ & ((1U << MLSD_TABLEBITS) - 1)];		\
	u64 w1_, e2_, x1_, x2_;						\
									\
	if (unlikely(e1_ & MLSD_SUB))					\
		e1_ = ADD_SUFFIX(mlsd_sub)(lt, e1_, w_, 16);		\
	w1_ = w_ >> (e1_ & 63);						\
	e2_ = ot[(w1_ & 0xFF) | (e1_ & MLSD_NOOFF)];			\
	if (unlikely(e2_ & MLSD_SUB))					\
		e2_ = ADD_SUFFIX(mlsd_sub)(ot, e2_, w1_, 8);		\
	x1_ = (w_ >> ((e1_ >> 16) & 63)) & ((u32)e1_ >> 27);		\
	x2_ = (w1_ >> ((e2_ >> 8) & 63)) & ((u32)e2_ >> 19);		\
	*(rp) = (e1_ >> 32) + (x1_ << 18) +				\
		((e2_ + (x2_ << 32)) & ~(u64)0xFFFFFFFF);		\
	*(pp) = (u16)(p);						\
	(p) += (u8)e1_ + (u8)e2_;					\
} while (0)

/* A range of verified records */
struct ADD_SUFFIX(mlsd_seg) {
	const u64 *p, *end;
};

/*
 * In mlsd_run(): execute up to 'r' records of the current range, unless 'out'
 * is past 'out_limit'.  Each one copies MLSD_COPY bytes, unless flagged slow.
 */
#define MLSD_EXEC_SOME()						\
do {									\
	const u64 *xm;							\
									\
	if (unlikely(out > out_limit))					\
		goto out;						\
	for (xm = xp + MIN(r, (size_t)(xe - xp)); xp != xm; xp++) {	\
		const u64 x = *xp;					\
		const u32 len = (x >> 18) & 0x1FF;			\
		const u8 *src;						\
									\
		put_unaligned_le16((u16)x, out);			\
		out += (x >> 16) & 3;					\
		src = out - ((x >> 32) & 0xFFFF);			\
		if (likely(!(x & MLSD_SLOW))) {				\
			memcpy(out, src, 16);				\
			memcpy(out + 16, src + 16, 16);			\
			memcpy(out + 32, src + 32, 16);			\
			memcpy(out + 48, src + 48, 16);			\
		} else {						\
			if (len == MLSD_EOB_LEN)			\
				goto out;				\
			COPY_MATCH(out, src, (x >> 32) & 0xFFFF,	\
				   out + len);				\
		}							\
		out += len;						\
	}								\
	if (xp == xe) {							\
		xp = ++k < nseg ? seg[k].p : NULL;			\
		xe = k < nseg ? seg[k].end : NULL;			\
	}								\
} while (0)

/*
 * Run the lanes, whose positions are pos[], for 'nsteps' steps, while executing
 * the records of seg[0..nseg-1], about 'r' per step, at *out_p.  Stop at an
 * end-of-block record, or when *out_p is past 'out_limit' (after which 'r' more
 * records must fit), and return a pointer to that record; else return NULL.
 * At least DEFLATE_MAX_MATCH_OFFSET bytes must have been written.
 */
static NOINLINE ATTRIBUTES const u64 *
ADD_SUFFIX(mlsd_run)(const u8 * restrict in, size_t * restrict pos,
		     const u64 * restrict lt, const u64 * restrict ot,
		     u64 (* restrict recs)[MLSD_NREC],
		     u16 (* restrict poss)[MLSD_NREC + 1], unsigned nsteps,
		     const struct ADD_SUFFIX(mlsd_seg) * restrict seg,
		     unsigned nseg, size_t r, u8 ** restrict out_p,
		     const u8 * restrict out_limit)
{
	size_t p[MLSD_LANES];
	u8 *out = *out_p;
	const u64 *xp = nseg ? seg[0].p : NULL, *xe = nseg ? seg[0].end : NULL;
	unsigned n, k = 0;
	int i;

	for (i = 0; i < MLSD_LANES; i++)
		p[i] = pos[i];
	for (n = 0; n < nsteps; n++) {
		MLSD_UNROLL_LOOP
		for (i = 0; i < MLSD_LANES; i++)
			MLSD_STEP(p[i], &recs[i][n], &poss[i][n]);
		if (xp != NULL)
			MLSD_EXEC_SOME();
	}
	while (xp != NULL)
		MLSD_EXEC_SOME();
	for (i = 0; i < MLSD_LANES; i++) {
		poss[i][nsteps] = (u16)p[i];
		pos[i] = p[i];
	}
out:
	*out_p = out;
	return xp;
}
#undef MLSD_EXEC_SOME

/*
 * Decode as much of the current Huffman block as possible with the lanes, and
 * leave ctx at a symbol boundary: past the end-of-block symbol if the result
 * is FASTLOOP_BLOCK_DONE.  At least DEFLATE_MAX_MATCH_OFFSET bytes must have
 * been written.  state[] carries the lanes' bits per step (times 16) and the
 * margin across blocks.
 */
static NOINLINE ATTRIBUTES enum fastloop_result
ADD_SUFFIX(decompress_multilane)(struct libdeflate_decompressor * restrict d,
				 struct fastloop_ctx * restrict ctx,
				 const u8 * const in, const u8 * const in_end,
				 u8 * const out_end, size_t * restrict state)
{
	const u64 * const lt = d->u.mlsd.lt, * const ot = d->u.mlsd.ot;
	size_t pos[2][MLSD_LANES], base[2][MLSD_LANES], nrec = 0, r = 0;
	size_t end_pos = (size_t)(ctx->in_next - in) * 8 - (u8)ctx->bitsleft;
	size_t rate16 = state[0], margin = state[1];
	struct ADD_SUFFIX(mlsd_seg) seg[MLSD_LANES];
	unsigned cur = 0, nseg = 0;
	u8 *out = ctx->out_next;
	enum fastloop_result res = FASTLOOP_BLOCK_NOT_DONE;
	int i;

	for (;;) {
		const size_t space = (r + 1) * MLSD_MAX_REC_OUT +
				     FASTLOOP_MAX_MATCH_OVERRUN;
		unsigned nsteps = nrec < MLSD_GROW ? MLSD_MIN_STEPS :
				  MLSD_ROUND_STEPS;
		const size_t dist = MAX(rate16 * nsteps / 16, margin + 64) -
				    margin;
		u16 (*pp)[MLSD_NREC + 1];
		unsigned ans[MLSD_LANES], tgt[MLSD_LANES], s, lo = 0;
		size_t prog = 0;
		const u64 *stop;

		/* Start a round of lanes, unless near the end of the input, */
		if (end_pos + (MLSD_LANES - 1) * dist +
		    (nsteps + MLSD_CATCHUP) * MLSD_MAX_STEP_BITS + 64 >
		    (size_t)(in_end - in) * 8) {
			if (nseg == 0)
				return res;	/* (ctx is unchanged) */
			nsteps = 0;
		}
		for (i = 0; i < MLSD_LANES; i++)
			pos[cur ^ 1][i] = base[cur ^ 1][i] =
				end_pos + (size_t)i * dist;
		/* ... while executing the verified records of the last one. */
		stop = ADD_SUFFIX(mlsd_run)(in, pos[cur ^ 1], lt, ot,
					    d->mlsd_recs[cur ^ 1],
					    d->mlsd_poss[cur ^ 1], nsteps, seg,
					    nseg, r, &out,
					    out_end - out > (ptrdiff_t)space ?
					    out_end - space : out - 1);
		if (stop != NULL) {
			/* At end-of-block, or out of space */
			const size_t idx = stop - &d->mlsd_recs[cur][0][0];
			const bool eob = (*stop >> 18 & 0x1FF) == MLSD_EOB_LEN;

			i = idx / MLSD_NREC;
			end_pos = base[cur][i] +
				  (u16)(d->mlsd_poss[cur][i][idx % MLSD_NREC +
							     eob] -
					base[cur][i]);
			if (eob)
				res = FASTLOOP_BLOCK_DONE;
			break;
		}
		if (nsteps == 0)
			break;
		cur ^= 1;

		/*
		 * Look for the end of each lane among the next lane's
		 * positions, with branchless binary searches run interleaved.
		 */
		pp = d->mlsd_poss[cur];
		for (i = 0; i + 1 < MLSD_LANES; i++) {
			const ptrdiff_t t = pos[cur][i] - base[cur][i + 1];

			tgt[i] = t <= 0 ? 0 : MIN(t, 0xFFFF);	/* 0: none */
			ans[i] = 0;
		}
		for (s = nsteps / 2; s >= 1; s >>= 1) {
			MLSD_UNROLL_LOOP
			for (i = 0; i + 1 < MLSD_LANES; i++) {
				const unsigned a = ans[i] + s;
				const u16 rel = pp[i + 1][a] - base[cur][i + 1];

				ans[i] = rel < tgt[i] ? a : ans[i];
			}
		}
		for (i = 0; i < MLSD_LANES; i++)
			prog += (u16)(pp[i][nsteps] - base[cur][i]);
		/*
		 * Collect the verified records: lane i+1 takes over from lane i
		 * where they meet, else lane i is extended until it lands on a
		 * position of lane i+1 (or the round ends with it).  The margin
		 * settles where 1/6 of the boundaries need such a catch-up.
		 */
		for (r = 0, nseg = 0, i = 0; ; i++) {
			unsigned cnt = nsteps;
			int next = -1;

			if (i + 1 < MLSD_LANES) {
				const size_t b = base[cur][i + 1];
				size_t p = pos[cur][i];
				unsigned h = ans[i] + 1;

				if (tgt[i] != 0 && (u16)(pp[i + 1][h] - b) ==
				    tgt[i]) {
					next = h;
					if (margin > 32)
						margin -= 4;
				} else {
					margin += 20;
				}
				while (next < 0 &&
				       cnt < nsteps + MLSD_CATCHUP) {
					MLSD_STEP(p, &d->mlsd_recs[cur][i][cnt],
						  &pp[i][cnt]);
					pp[i][++cnt] = (u16)p;
					if (p <= b)
						continue;
					while (h <= nsteps &&
					       (u16)(pp[i + 1][h] - b) < p - b)
						h++;
					if (h > nsteps)
						break;
					if ((u16)(pp[i + 1][h] - b) == p - b)
						next = h;
				}
				pos[cur][i] = p;
			}
			if (cnt > lo) {
				seg[nseg].p = &d->mlsd_recs[cur][i][lo];
				seg[nseg].end = &d->mlsd_recs[cur][i][cnt];
				r += cnt - lo;
				nseg++;
			}
			if (next < 0)
				break;
			lo = next;
		}
		end_pos = pos[cur][i];
		rate16 = prog * 16 / (MLSD_LANES * nsteps);
		margin = MIN(margin, 1024);
		nrec += r;
		nsteps = nrec < MLSD_GROW ? MLSD_MIN_STEPS : MLSD_ROUND_STEPS;
		r = (r + nsteps - 1) / nsteps;
	}
	state[0] = rate16;
	state[1] = margin;
	ctx->bitbuf = get_unaligned_leword(in + (end_pos >> 3)) >>
		      (end_pos & 7);
	ctx->in_next = in + (end_pos >> 3) + sizeof(bitbuf_t) - 1;
	ctx->bitsleft = (MAX_BITSLEFT & ~7) - (end_pos & 7);
	ctx->out_next = out;
	return res;
}
