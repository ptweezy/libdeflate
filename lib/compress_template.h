/*
 * compress_template.h - template for the DEFLATE compressors of levels 1 to 9
 *
 * Copyright 2016 Eric Biggers
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * This file is a template parameterized by SUFFIX and SMALL_INPUT.
 * deflate_compress.c includes it twice with different "arguments", so it
 * intentionally has no include guard.
 *
 * It implements deflate_compress_fastest(), deflate_compress_greedy(),
 * deflate_compress_lazy() and deflate_compress_lazy2().
 *
 * SUFFIX:
 *	Name suffix to append to all instantiated functions.
 * SMALL_INPUT:
 *	If 0, instantiate the functions for inputs in general.
 *	If 1, instantiate the functions for small inputs.  They use the
 *	matchfinder functions for small inputs, which initialize and search a
 *	small map in place of each hash table; see "Small inputs" in
 *	matchfinder_common.h.  The input must be at most
 *	HT_MATCHFINDER_MAX_SMALL_INPUT_LENGTH bytes for
 *	deflate_compress_fastest() and at most
 *	HC_MATCHFINDER_MAX_SMALL_INPUT_LENGTH bytes for the others.
 */

/* The additional argument of the matchfinder functions for small inputs */
#if SMALL_INPUT
#  define SMALL_ORDER_ARG	, order
#else
#  define SMALL_ORDER_ARG
#endif

/*
 * This is a faster variant of deflate_compress_greedy().  It uses the
 * ht_matchfinder rather than the hc_matchfinder.  It also skips the block
 * splitting algorithm and just uses fixed length blocks.  c->max_search_depth
 * has no effect with this algorithm, as it is hardcoded in ht_matchfinder.h.
 */
static void
ADD_SUFFIX(deflate_compress_fastest)(struct libdeflate_compressor * restrict c,
				     const u8 *in, size_t in_nbytes,
				     struct deflate_output_bitstream *os)
{
	const u8 *in_next = in;
	const u8 *in_end = in_next + in_nbytes;
	const u8 *in_cur_base = in_next;
	u32 max_len = DEFLATE_MAX_MATCH_LEN;
	u32 nice_len = MIN(c->nice_match_length, max_len);
	u32 next_hash = 0;
#if SMALL_INPUT
	const unsigned order = matchfinder_small_order(in_nbytes);
#endif

	ADD_SUFFIX(ht_matchfinder_init)(&c->p.f.ht_mf SMALL_ORDER_ARG);

	do {
		/* Starting a new DEFLATE block */

		const u8 * const in_block_begin = in_next;
		const u8 * const in_max_block_end = choose_max_block_end(
				in_next, in_end, FAST_SOFT_MAX_BLOCK_LENGTH);
		struct deflate_sequence *seq = c->p.f.sequences;

		deflate_begin_sequences(c, seq);

		do {
			u32 length;
			u32 offset;
			size_t remaining = in_end - in_next;

			if (unlikely(remaining < DEFLATE_MAX_MATCH_LEN)) {
				max_len = remaining;
				if (max_len < HT_MATCHFINDER_REQUIRED_NBYTES) {
					do {
						deflate_choose_literal(c,
							*in_next++, false, seq);
					} while (--max_len);
					break;
				}
				nice_len = MIN(nice_len, max_len);
			}
			length = ADD_SUFFIX(ht_matchfinder_longest_match)(
						&c->p.f.ht_mf, &in_cur_base,
						in_next, max_len, nice_len,
						&next_hash, &offset
						SMALL_ORDER_ARG);
			if (length) {
				/* Match found */
				deflate_choose_match(c, length, offset, false,
						     &seq);
				ADD_SUFFIX(ht_matchfinder_skip_bytes)(
							&c->p.f.ht_mf,
							&in_cur_base,
							in_next + 1, in_end,
							length - 1, &next_hash
							SMALL_ORDER_ARG);
				in_next += length;
			} else {
				/* No match found */
				deflate_choose_literal(c, *in_next++, false,
						       seq);
			}

			/* Check if it's time to output another block. */
		} while (in_next < in_max_block_end &&
			 seq < &c->p.f.sequences[FAST_SEQ_STORE_LENGTH]);

		deflate_finish_block(c, os, in_block_begin,
				     in_next - in_block_begin,
				     c->p.f.sequences, in_next == in_end);
	} while (in_next != in_end && !os->overflow);
}

/*
 * This is the "greedy" DEFLATE compressor. It always chooses the longest match.
 */
static void
ADD_SUFFIX(deflate_compress_greedy)(struct libdeflate_compressor * restrict c,
				    const u8 *in, size_t in_nbytes,
				    struct deflate_output_bitstream *os)
{
	const u8 *in_next = in;
	const u8 *in_end = in_next + in_nbytes;
	const u8 *in_cur_base = in_next;
	u32 max_len = DEFLATE_MAX_MATCH_LEN;
	u32 nice_len = MIN(c->nice_match_length, max_len);
	u32 next_hashes[2] = {0, 0};
#if SMALL_INPUT
	const unsigned order = matchfinder_small_order(in_nbytes);
#endif

	ADD_SUFFIX(hc_matchfinder_init)(&c->p.g.hc_mf, GREEDY_MULTI_HOP
					SMALL_ORDER_ARG);

	do {
		/* Starting a new DEFLATE block */

		const u8 * const in_block_begin = in_next;
		const u8 * const in_max_block_end = choose_max_block_end(
				in_next, in_end, SOFT_MAX_BLOCK_LENGTH);
		struct deflate_sequence *seq = c->p.g.sequences;
		u32 min_len;

		init_block_split_stats(&c->split_stats);
		deflate_begin_sequences(c, seq);
		min_len = calculate_min_match_len(in_next,
						  in_max_block_end - in_next,
						  c->max_search_depth);
		do {
			u32 length;
			u32 offset;

			adjust_max_and_nice_len(&max_len, &nice_len,
						in_end - in_next);
			length = ADD_SUFFIX(hc_matchfinder_longest_match)(
						&c->p.g.hc_mf, &in_cur_base,
						in_next, min_len - 1, max_len,
						nice_len, c->max_search_depth,
						next_hashes, &offset,
						GREEDY_MULTI_HOP
						SMALL_ORDER_ARG);

			if (length >= min_len &&
			    (length > DEFLATE_MIN_MATCH_LEN ||
			     offset <= 4096)) {
				/* Match found */
				deflate_choose_match(c, length, offset, true,
						     &seq);
				ADD_SUFFIX(hc_matchfinder_skip_bytes)(
							&c->p.g.hc_mf,
							&in_cur_base,
							in_next + 1, in_end,
							length - 1, next_hashes,
							GREEDY_MULTI_HOP
							SMALL_ORDER_ARG);
				in_next += length;
			} else {
				/* No match found */
				deflate_choose_literal(c, *in_next++, true,
						       seq);
			}

			/* Check if it's time to output another block. */
		} while (in_next < in_max_block_end &&
			 seq < &c->p.g.sequences[SEQ_STORE_LENGTH] &&
			 !should_end_block(&c->split_stats,
					   in_block_begin, in_next, in_end));

		deflate_finish_block(c, os, in_block_begin,
				     in_next - in_block_begin,
				     c->p.g.sequences, in_next == in_end);
	} while (in_next != in_end && !os->overflow);
}

static forceinline void
ADD_SUFFIX(deflate_compress_lazy_generic)(
			struct libdeflate_compressor * restrict c,
			const u8 *in, size_t in_nbytes,
			struct deflate_output_bitstream *os, bool lazy2)
{
	const u8 *in_next = in;
	const u8 *in_end = in_next + in_nbytes;
	const u8 *in_cur_base = in_next;
	u32 max_len = DEFLATE_MAX_MATCH_LEN;
	u32 nice_len = MIN(c->nice_match_length, max_len);
	u32 next_hashes[2] = {0, 0};
#if SMALL_INPUT
	const unsigned order = matchfinder_small_order(in_nbytes);
#endif

	ADD_SUFFIX(hc_matchfinder_init)(&c->p.g.hc_mf, LAZY_MULTI_HOP
					SMALL_ORDER_ARG);

	do {
		/* Starting a new DEFLATE block */

		const u8 * const in_block_begin = in_next;
		const u8 * const in_max_block_end = choose_max_block_end(
				in_next, in_end, SOFT_MAX_BLOCK_LENGTH);
#if !SMALL_INPUT
		const u8 *next_recalc_min_len =
			in_next + MIN(in_end - in_next, 10000);
#endif
		struct deflate_sequence *seq = c->p.g.sequences;
		u32 min_len;

		init_block_split_stats(&c->split_stats);
		deflate_begin_sequences(c, seq);
		min_len = calculate_min_match_len(in_next,
						  in_max_block_end - in_next,
						  c->max_search_depth);
		do {
			u32 cur_len;
			u32 cur_offset;
			u32 next_len;
			u32 next_offset;

			/*
			 * Recalculate the minimum match length if it hasn't
			 * been done recently.  (A small input ends before the
			 * first recalculation.)
			 */
#if SMALL_INPUT
			STATIC_ASSERT(HC_MATCHFINDER_MAX_SMALL_INPUT_LENGTH <=
				      10000);
#else
			if (in_next >= next_recalc_min_len) {
				min_len = recalculate_min_match_len(
						&c->freqs,
						c->max_search_depth);
				next_recalc_min_len +=
					MIN(in_end - next_recalc_min_len,
					    in_next - in_block_begin);
			}
#endif

			/* Find the longest match at the current position. */
			adjust_max_and_nice_len(&max_len, &nice_len,
						in_end - in_next);
			cur_len = ADD_SUFFIX(hc_matchfinder_longest_match)(
						&c->p.g.hc_mf, &in_cur_base,
						in_next, min_len - 1, max_len,
						nice_len, c->max_search_depth,
						next_hashes, &cur_offset,
						LAZY_MULTI_HOP
						SMALL_ORDER_ARG);
			if (cur_len < min_len ||
			    (cur_len == DEFLATE_MIN_MATCH_LEN &&
			     cur_offset > 8192)) {
				/* No match found.  Choose a literal. */
				deflate_choose_literal(c, *in_next++, true,
						       seq);
				continue;
			}
			in_next++;

have_cur_match:
			/*
			 * We have a match at the current position.
			 * If it's very long, choose it immediately.
			 */
			if (cur_len >= nice_len) {
				deflate_choose_match(c, cur_len, cur_offset,
						     true, &seq);
				ADD_SUFFIX(hc_matchfinder_skip_bytes)(
							&c->p.g.hc_mf,
							&in_cur_base, in_next,
							in_end, cur_len - 1,
							next_hashes,
							LAZY_MULTI_HOP
							SMALL_ORDER_ARG);
				in_next += cur_len - 1;
				continue;
			}

			/*
			 * Try to find a better match at the next position.
			 *
			 * Note: since we already have a match at the *current*
			 * position, we use only half the 'max_search_depth'
			 * when checking the *next* position.  This is a useful
			 * trade-off because it's more worthwhile to use a
			 * greater search depth on the initial match.
			 *
			 * Note: it's possible to structure the code such that
			 * there's only one call to longest_match(), which
			 * handles both the "find the initial match" and "try to
			 * find a better match" cases.  However, it is faster to
			 * have two call sites, with longest_match() inlined at
			 * each.
			 */
			adjust_max_and_nice_len(&max_len, &nice_len,
						in_end - in_next);
			next_len = ADD_SUFFIX(hc_matchfinder_longest_match)(
						&c->p.g.hc_mf, &in_cur_base,
						in_next++, cur_len - 1, max_len,
						nice_len,
						c->max_search_depth >> 1,
						next_hashes, &next_offset,
						LAZY_MULTI_HOP
						SMALL_ORDER_ARG);
			if (next_len >= cur_len &&
			    4 * (int)(next_len - cur_len) +
			    ((int)bsr32(cur_offset) -
			     (int)bsr32(next_offset)) > 2) {
				/*
				 * Found a better match at the next position.
				 * Output a literal.  Then the next match
				 * becomes the current match.
				 */
				deflate_choose_literal(c, *(in_next - 2), true,
						       seq);
				cur_len = next_len;
				cur_offset = next_offset;
				goto have_cur_match;
			}

			if (lazy2) {
				/* In lazy2 mode, look ahead another position */
				adjust_max_and_nice_len(&max_len, &nice_len,
							in_end - in_next);
				next_len =
					ADD_SUFFIX(hc_matchfinder_longest_match)(
							&c->p.g.hc_mf,
							&in_cur_base, in_next++,
							cur_len - 1, max_len,
							nice_len,
							c->max_search_depth >> 2,
							next_hashes,
							&next_offset,
							LAZY_MULTI_HOP
							SMALL_ORDER_ARG);
				if (next_len >= cur_len &&
				    4 * (int)(next_len - cur_len) +
				    ((int)bsr32(cur_offset) -
				     (int)bsr32(next_offset)) > 6) {
					/*
					 * There's a much better match two
					 * positions ahead, so use two literals.
					 */
					deflate_choose_literal(
						c, *(in_next - 3), true, seq);
					deflate_choose_literal(
						c, *(in_next - 2), true, seq);
					cur_len = next_len;
					cur_offset = next_offset;
					goto have_cur_match;
				}
				/*
				 * No better match at either of the next 2
				 * positions.  Output the current match.
				 */
				deflate_choose_match(c, cur_len, cur_offset,
						     true, &seq);
				if (cur_len > 3) {
					ADD_SUFFIX(hc_matchfinder_skip_bytes)(
							&c->p.g.hc_mf,
							&in_cur_base, in_next,
							in_end, cur_len - 3,
							next_hashes,
							LAZY_MULTI_HOP
							SMALL_ORDER_ARG);
					in_next += cur_len - 3;
				}
			} else { /* !lazy2 */
				/*
				 * No better match at the next position.  Output
				 * the current match.
				 */
				deflate_choose_match(c, cur_len, cur_offset,
						     true, &seq);
				ADD_SUFFIX(hc_matchfinder_skip_bytes)(
							&c->p.g.hc_mf,
							&in_cur_base, in_next,
							in_end, cur_len - 2,
							next_hashes,
							LAZY_MULTI_HOP
							SMALL_ORDER_ARG);
				in_next += cur_len - 2;
			}
			/* Check if it's time to output another block. */
		} while (in_next < in_max_block_end &&
			 seq < &c->p.g.sequences[SEQ_STORE_LENGTH] &&
			 !should_end_block(&c->split_stats,
					   in_block_begin, in_next, in_end));

		deflate_finish_block(c, os, in_block_begin,
				     in_next - in_block_begin,
				     c->p.g.sequences, in_next == in_end);
	} while (in_next != in_end && !os->overflow);
}

/*
 * This is the "lazy" DEFLATE compressor.  Before choosing a match, it checks to
 * see if there's a better match at the next position.  If yes, it outputs a
 * literal and continues to the next position.  If no, it outputs the match.
 */
static void
ADD_SUFFIX(deflate_compress_lazy)(struct libdeflate_compressor * restrict c,
				  const u8 *in, size_t in_nbytes,
				  struct deflate_output_bitstream *os)
{
	ADD_SUFFIX(deflate_compress_lazy_generic)(c, in, in_nbytes, os, false);
}

/*
 * The lazy2 compressor.  This is similar to the regular lazy one, but it looks
 * for a better match at the next 2 positions rather than the next 1.  This
 * makes it take slightly more time, but compress some inputs slightly more.
 */
static void
ADD_SUFFIX(deflate_compress_lazy2)(struct libdeflate_compressor * restrict c,
				   const u8 *in, size_t in_nbytes,
				   struct deflate_output_bitstream *os)
{
	ADD_SUFFIX(deflate_compress_lazy_generic)(c, in, in_nbytes, os, true);
}

#undef SMALL_ORDER_ARG

#undef SUFFIX
#undef SMALL_INPUT
