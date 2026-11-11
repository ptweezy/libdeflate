/*
 * compress_template.h - the parsers of the DEFLATE compressor
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
 * These are the functions that choose the matches and literals, lifted out of
 * deflate_compress.c so that they can be compiled twice.
 *
 * The first time, they are the functions that libdeflate_deflate_compress()
 * uses for most inputs.  They initialize the matchfinder.
 *
 * The second time, MATCHFINDER_CONTINUED is defined, and macros rename the
 * functions to *_continued().  These variants are for small inputs, for which
 * initializing the matchfinder would take a large part of the time.  They
 * don't initialize it.  Instead, the matchfinder positions of the input
 * buffer follow those of the previous buffer (c->mf_next_pos), and the
 * matchfinder ignores the positions of earlier buffers.  The matches found, and
 * therefore the compressed data, are the same as with the first variant.
 *
 * Each variant has its own copies of the small helper functions in this file,
 * and the continued variants always inline their copies.  This leaves the
 * compiler's inlining decisions for the first variants as they would be if
 * they were the only ones.
 */

#ifdef MATCHFINDER_CONTINUED
/*
 * The inputs of the continued variants are shorter than MIN_BLOCK_LENGTH, so
 * the block splitting algorithm can't end a block.  Skip its checks.
 */
#  define should_end_block(stats, in_block_begin, in_next, in_end)	false
#  define ready_to_check_block(stats, in_block_begin, in_next, in_end)	false
#endif

#ifdef MATCHFINDER_CONTINUED
static forceinline void
#else
static void
#endif
deflate_finish_block(struct libdeflate_compressor *c,
		     struct deflate_output_bitstream *os,
		     const u8 *block_begin, u32 block_length,
		     const struct deflate_sequence *sequences,
		     bool is_final_block)
{
	c->freqs.litlen[DEFLATE_END_OF_BLOCK]++;
	deflate_make_huffman_codes(&c->freqs, &c->codes);
	deflate_flush_block(c, os, block_begin, block_length, sequences,
			    is_final_block);
}


/*
 * This is a faster variant of deflate_compress_greedy().  It uses the
 * ht_matchfinder rather than the hc_matchfinder.  It also skips the block
 * splitting algorithm and just uses fixed length blocks.  c->max_search_depth
 * has no effect with this algorithm, as it is hardcoded in ht_matchfinder.h.
 */
static void
deflate_compress_fastest(struct libdeflate_compressor * restrict c,
			 const u8 *in, size_t in_nbytes,
			 struct deflate_output_bitstream *os)
{
	const u8 *in_next = in;
	const u8 *in_end = in_next + in_nbytes;
#ifdef MATCHFINDER_CONTINUED
	const struct matchfinder_continuation in_cur_base = {
		in, c->mf_next_pos
	};
#else
	const u8 *in_cur_base = in_next;
#endif
	u32 max_len = DEFLATE_MAX_MATCH_LEN;
	u32 nice_len = MIN(c->nice_match_length, max_len);
	u32 next_hash = 0;

#ifndef MATCHFINDER_CONTINUED
	ht_matchfinder_init(&c->p.f.ht_mf);
#endif

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
			length = ht_matchfinder_longest_match(&c->p.f.ht_mf,
							      &in_cur_base,
							      in_next,
							      max_len,
							      nice_len,
							      &next_hash,
							      &offset);
			if (length) {
				/* Match found */
				deflate_choose_match(c, length, offset, false,
						     &seq);
				ht_matchfinder_skip_bytes(&c->p.f.ht_mf,
							  &in_cur_base,
							  in_next + 1,
							  in_end,
							  length - 1,
							  &next_hash);
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
deflate_compress_greedy(struct libdeflate_compressor * restrict c,
			const u8 *in, size_t in_nbytes,
			struct deflate_output_bitstream *os)
{
	const u8 *in_next = in;
	const u8 *in_end = in_next + in_nbytes;
#ifdef MATCHFINDER_CONTINUED
	const struct matchfinder_continuation in_cur_base = {
		in, c->mf_next_pos
	};
#else
	const u8 *in_cur_base = in_next;
#endif
	u32 max_len = DEFLATE_MAX_MATCH_LEN;
	u32 nice_len = MIN(c->nice_match_length, max_len);
	u32 next_hashes[2] = {0, 0};

#ifndef MATCHFINDER_CONTINUED
	hc_matchfinder_init(&c->p.g.hc_mf, GREEDY_MULTI_HOP);
#endif

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
			length = hc_matchfinder_longest_match(
						&c->p.g.hc_mf,
						&in_cur_base,
						in_next,
						min_len - 1,
						max_len,
						nice_len,
						c->max_search_depth,
						next_hashes,
						&offset,
						GREEDY_MULTI_HOP);

			if (length >= min_len &&
			    (length > DEFLATE_MIN_MATCH_LEN ||
			     offset <= 4096)) {
				/* Match found */
				deflate_choose_match(c, length, offset, true,
						     &seq);
				hc_matchfinder_skip_bytes(&c->p.g.hc_mf,
							  &in_cur_base,
							  in_next + 1,
							  in_end,
							  length - 1,
							  next_hashes,
							  GREEDY_MULTI_HOP);
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
deflate_compress_lazy_generic(struct libdeflate_compressor * restrict c,
			      const u8 *in, size_t in_nbytes,
			      struct deflate_output_bitstream *os, bool lazy2)
{
	const u8 *in_next = in;
	const u8 *in_end = in_next + in_nbytes;
#ifdef MATCHFINDER_CONTINUED
	const struct matchfinder_continuation in_cur_base = {
		in, c->mf_next_pos
	};
#else
	const u8 *in_cur_base = in_next;
#endif
	u32 max_len = DEFLATE_MAX_MATCH_LEN;
	u32 nice_len = MIN(c->nice_match_length, max_len);
	u32 next_hashes[2] = {0, 0};

#ifdef MATCHFINDER_CONTINUED
	/* The input is too short for min_len to be recalculated below. */
	STATIC_ASSERT(MAX_CONTINUED_INPUT_LENGTH < 10000);
#else
	hc_matchfinder_init(&c->p.g.hc_mf, LAZY_MULTI_HOP);
#endif

	do {
		/* Starting a new DEFLATE block */

		const u8 * const in_block_begin = in_next;
		const u8 * const in_max_block_end = choose_max_block_end(
				in_next, in_end, SOFT_MAX_BLOCK_LENGTH);
#ifndef MATCHFINDER_CONTINUED
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

#ifndef MATCHFINDER_CONTINUED
			/*
			 * Recalculate the minimum match length if it hasn't
			 * been done recently.
			 */
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
			cur_len = hc_matchfinder_longest_match(
						&c->p.g.hc_mf,
						&in_cur_base,
						in_next,
						min_len - 1,
						max_len,
						nice_len,
						c->max_search_depth,
						next_hashes,
						&cur_offset,
						LAZY_MULTI_HOP);
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
				hc_matchfinder_skip_bytes(&c->p.g.hc_mf,
							  &in_cur_base,
							  in_next,
							  in_end,
							  cur_len - 1,
							  next_hashes,
							  LAZY_MULTI_HOP);
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
			next_len = hc_matchfinder_longest_match(
						&c->p.g.hc_mf,
						&in_cur_base,
						in_next++,
						cur_len - 1,
						max_len,
						nice_len,
						c->max_search_depth >> 1,
						next_hashes,
						&next_offset,
						LAZY_MULTI_HOP);
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
				next_len = hc_matchfinder_longest_match(
						&c->p.g.hc_mf,
						&in_cur_base,
						in_next++,
						cur_len - 1,
						max_len,
						nice_len,
						c->max_search_depth >> 2,
						next_hashes,
						&next_offset,
						LAZY_MULTI_HOP);
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
					hc_matchfinder_skip_bytes(&c->p.g.hc_mf,
								  &in_cur_base,
								  in_next,
								  in_end,
								  cur_len - 3,
								  next_hashes,
								  LAZY_MULTI_HOP);
					in_next += cur_len - 3;
				}
			} else { /* !lazy2 */
				/*
				 * No better match at the next position.  Output
				 * the current match.
				 */
				deflate_choose_match(c, cur_len, cur_offset,
						     true, &seq);
				hc_matchfinder_skip_bytes(&c->p.g.hc_mf,
							  &in_cur_base,
							  in_next,
							  in_end,
							  cur_len - 2,
							  next_hashes,
							  LAZY_MULTI_HOP);
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
deflate_compress_lazy(struct libdeflate_compressor * restrict c,
		      const u8 *in, size_t in_nbytes,
		      struct deflate_output_bitstream *os)
{
	deflate_compress_lazy_generic(c, in, in_nbytes, os, false);
}

/*
 * The lazy2 compressor.  This is similar to the regular lazy one, but it looks
 * for a better match at the next 2 positions rather than the next 1.  This
 * makes it take slightly more time, but compress some inputs slightly more.
 */
static void
deflate_compress_lazy2(struct libdeflate_compressor * restrict c,
		       const u8 *in, size_t in_nbytes,
		       struct deflate_output_bitstream *os)
{
	deflate_compress_lazy_generic(c, in, in_nbytes, os, true);
}

#if SUPPORT_NEAR_OPTIMAL_PARSING

#ifdef MATCHFINDER_CONTINUED
static forceinline void
#else
static void
#endif
deflate_near_optimal_init_stats(struct libdeflate_compressor *c)
{
	init_block_split_stats(&c->split_stats);
	memset(c->p.n.new_match_len_freqs, 0,
	       sizeof(c->p.n.new_match_len_freqs));
	memset(c->p.n.match_len_freqs, 0, sizeof(c->p.n.match_len_freqs));
}

#ifdef MATCHFINDER_CONTINUED
static forceinline void
#else
static void
#endif
deflate_near_optimal_merge_stats(struct libdeflate_compressor *c)
{
	unsigned i;

	merge_new_observations(&c->split_stats);
	for (i = 0; i < ARRAY_LEN(c->p.n.match_len_freqs); i++) {
		c->p.n.match_len_freqs[i] += c->p.n.new_match_len_freqs[i];
		c->p.n.new_match_len_freqs[i] = 0;
	}
}

/*
 * Save some literal/match statistics from the previous block so that
 * deflate_adjust_costs() will be able to decide how much the current block
 * differs from the previous one.
 */
#ifdef MATCHFINDER_CONTINUED
static forceinline void
#else
static void
#endif
deflate_near_optimal_save_stats(struct libdeflate_compressor *c)
{
	int i;

	for (i = 0; i < NUM_OBSERVATION_TYPES; i++)
		c->p.n.prev_observations[i] = c->split_stats.observations[i];
	c->p.n.prev_num_observations = c->split_stats.num_observations;
}

#ifdef MATCHFINDER_CONTINUED
static forceinline void
#else
static void
#endif
deflate_near_optimal_clear_old_stats(struct libdeflate_compressor *c)
{
	int i;

	for (i = 0; i < NUM_OBSERVATION_TYPES; i++)
		c->split_stats.observations[i] = 0;
	c->split_stats.num_observations = 0;
	memset(c->p.n.match_len_freqs, 0, sizeof(c->p.n.match_len_freqs));
}

/*
 * This is the "near-optimal" DEFLATE compressor.  It computes the optimal
 * representation of each DEFLATE block using a minimum-cost path search over
 * the graph of possible match/literal choices for that block, assuming a
 * certain cost for each Huffman symbol.
 *
 * For several reasons, the end result is not guaranteed to be optimal:
 *
 * - Nonoptimal choice of blocks
 * - Heuristic limitations on which matches are actually considered
 * - Symbol costs are unknown until the symbols have already been chosen
 *   (so iterative optimization must be used)
 */
static void
deflate_compress_near_optimal(struct libdeflate_compressor * restrict c,
			      const u8 *in, size_t in_nbytes,
			      struct deflate_output_bitstream *os)
{
	const u8 *in_next = in;
	const u8 *in_block_begin = in_next;
	const u8 *in_end = in_next + in_nbytes;
	const u8 *in_cur_base = in_next;
	const u8 *in_next_slide =
		in_next + MIN(in_end - in_next, MATCHFINDER_WINDOW_SIZE);
	u32 max_len = DEFLATE_MAX_MATCH_LEN;
	u32 nice_len = MIN(c->nice_match_length, max_len);
	struct lz_match *cache_ptr = c->p.n.match_cache;
	u32 next_hashes[2] = {0, 0};
	bool prev_block_used_only_literals = false;

#ifndef MATCHFINDER_CONTINUED
	bt_matchfinder_init(&c->p.n.bt_mf);
#endif
	deflate_near_optimal_init_stats(c);

	do {
		/* Starting a new DEFLATE block */
		const u8 * const in_max_block_end = choose_max_block_end(
				in_block_begin, in_end, SOFT_MAX_BLOCK_LENGTH);
		const u8 *prev_end_block_check = NULL;
		bool change_detected = false;
		const u8 *next_observation = in_next;
		u32 min_len;

		/*
		 * Use the minimum match length heuristic to improve the
		 * literal/match statistics gathered during matchfinding.
		 * However, the actual near-optimal parse won't respect min_len,
		 * as it can accurately assess the costs of different matches.
		 *
		 * If the "use only literals" strategy happened to be the best
		 * strategy on the previous block, then probably the
		 * min_match_len heuristic is still not aggressive enough for
		 * the data, so force gathering literal stats only.
		 */
		if (prev_block_used_only_literals)
			min_len = DEFLATE_MAX_MATCH_LEN + 1;
		else
			min_len = calculate_min_match_len(
					in_block_begin,
					in_max_block_end - in_block_begin,
					c->max_search_depth);

		/*
		 * Find matches until we decide to end the block.  We end the
		 * block if any of the following is true:
		 *
		 * (1) Maximum block length has been reached
		 * (2) Match catch may overflow.
		 * (3) Block split heuristic says to split now.
		 */
		for (;;) {
			struct lz_match *matches;
			u32 best_len;
			size_t remaining = in_end - in_next;

			/* Slide the window forward if needed. */
			if (in_next == in_next_slide) {
				bt_matchfinder_slide_window(&c->p.n.bt_mf);
				in_cur_base = in_next;
				in_next_slide = in_next +
					MIN(remaining, MATCHFINDER_WINDOW_SIZE);
			}

			/*
			 * Find matches with the current position using the
			 * binary tree matchfinder and save them in match_cache.
			 *
			 * Note: the binary tree matchfinder is more suited for
			 * optimal parsing than the hash chain matchfinder.  The
			 * reasons for this include:
			 *
			 * - The binary tree matchfinder can find more matches
			 *   in the same number of steps.
			 * - One of the major advantages of hash chains is that
			 *   skipping positions (not searching for matches at
			 *   them) is faster; however, with optimal parsing we
			 *   search for matches at almost all positions, so this
			 *   advantage of hash chains is negated.
			 */
			matches = cache_ptr;
			best_len = 0;
			adjust_max_and_nice_len(&max_len, &nice_len, remaining);
			if (likely(max_len >= BT_MATCHFINDER_REQUIRED_NBYTES)) {
				cache_ptr = bt_matchfinder_get_matches(
						&c->p.n.bt_mf,
						in_cur_base,
						in_next - in_cur_base,
#ifdef MATCHFINDER_CONTINUED
						c->mf_next_pos,
#endif
						max_len,
						nice_len,
						c->max_search_depth,
						next_hashes,
						matches);
				if (cache_ptr > matches)
					best_len = cache_ptr[-1].length;
			}
			if (in_next >= next_observation) {
				if (best_len >= min_len) {
					observe_match(&c->split_stats,
						      best_len);
					next_observation = in_next + best_len;
					c->p.n.new_match_len_freqs[best_len]++;
				} else {
					observe_literal(&c->split_stats,
							*in_next);
					next_observation = in_next + 1;
				}
			}

			cache_ptr->length = cache_ptr - matches;
			cache_ptr->offset = *in_next;
			in_next++;
			cache_ptr++;

			/*
			 * If there was a very long match found, don't cache any
			 * matches for the bytes covered by that match.  This
			 * avoids degenerate behavior when compressing highly
			 * redundant data, where the number of matches can be
			 * very large.
			 *
			 * This heuristic doesn't actually hurt the compression
			 * ratio very much.  If there's a long match, then the
			 * data must be highly compressible, so it doesn't
			 * matter much what we do.
			 */
			if (best_len >= DEFLATE_MIN_MATCH_LEN &&
			    best_len >= nice_len) {
				--best_len;
				do {
					remaining = in_end - in_next;
					if (in_next == in_next_slide) {
						bt_matchfinder_slide_window(
							&c->p.n.bt_mf);
						in_cur_base = in_next;
						in_next_slide = in_next +
							MIN(remaining,
							    MATCHFINDER_WINDOW_SIZE);
					}
					adjust_max_and_nice_len(&max_len,
								&nice_len,
								remaining);
					if (max_len >=
					    BT_MATCHFINDER_REQUIRED_NBYTES) {
						bt_matchfinder_skip_byte(
							&c->p.n.bt_mf,
							in_cur_base,
							in_next - in_cur_base,
#ifdef MATCHFINDER_CONTINUED
							c->mf_next_pos,
#endif
							nice_len,
							c->max_search_depth,
							next_hashes);
					}
					cache_ptr->length = 0;
					cache_ptr->offset = *in_next;
					in_next++;
					cache_ptr++;
				} while (--best_len);
			}
			/* Maximum block length or end of input reached? */
			if (in_next >= in_max_block_end)
				break;
			/* Match cache overflowed? */
			if (cache_ptr >=
			    &c->p.n.match_cache[MATCH_CACHE_LENGTH])
				break;
			/* Not ready to try to end the block (again)? */
			if (!ready_to_check_block(&c->split_stats,
						  in_block_begin, in_next,
						  in_end))
				continue;
			/* Check if it would be worthwhile to end the block. */
			if (do_end_block_check(&c->split_stats,
					       in_next - in_block_begin)) {
				change_detected = true;
				break;
			}
			/* Ending the block doesn't seem worthwhile here. */
			deflate_near_optimal_merge_stats(c);
			prev_end_block_check = in_next;
		}
		/*
		 * All the matches for this block have been cached.  Now choose
		 * the precise end of the block and the sequence of items to
		 * output to represent it, then flush the block.
		 */
		if (change_detected && prev_end_block_check != NULL) {
			/*
			 * The block is being ended because a recent chunk of
			 * data differs from the rest of the block.  We could
			 * end the block at 'in_next' like the greedy and lazy
			 * compressors do, but that's not ideal since it would
			 * include the differing chunk in the block.  The
			 * near-optimal compressor has time to do a better job.
			 * Therefore, we rewind to just before the chunk, and
			 * output a block that only goes up to there.
			 *
			 * We then set things up to correctly start the next
			 * block, considering that some work has already been
			 * done on it (some matches found and stats gathered).
			 */
			struct lz_match *orig_cache_ptr = cache_ptr;
			const u8 *in_block_end = prev_end_block_check;
			u32 block_length = in_block_end - in_block_begin;
			bool is_first = (in_block_begin == in);
			bool is_final = false;
			u32 num_bytes_to_rewind = in_next - in_block_end;
			size_t cache_len_rewound;

			/* Rewind the match cache. */
			do {
				cache_ptr--;
				cache_ptr -= cache_ptr->length;
			} while (--num_bytes_to_rewind);
			cache_len_rewound = orig_cache_ptr - cache_ptr;

			deflate_optimize_and_flush_block(
						c, os, in_block_begin,
						block_length, cache_ptr,
						is_first, is_final,
						&prev_block_used_only_literals);
			memmove(c->p.n.match_cache, cache_ptr,
				cache_len_rewound * sizeof(*cache_ptr));
			cache_ptr = &c->p.n.match_cache[cache_len_rewound];
			deflate_near_optimal_save_stats(c);
			/*
			 * Clear the stats for the just-flushed block, leaving
			 * just the stats for the beginning of the next block.
			 */
			deflate_near_optimal_clear_old_stats(c);
			in_block_begin = in_block_end;
		} else {
			/*
			 * The block is being ended for a reason other than a
			 * differing data chunk being detected.  Don't rewind at
			 * all; just end the block at the current position.
			 */
			u32 block_length = in_next - in_block_begin;
			bool is_first = (in_block_begin == in);
			bool is_final = (in_next == in_end);

			deflate_near_optimal_merge_stats(c);
			deflate_optimize_and_flush_block(
						c, os, in_block_begin,
						block_length, cache_ptr,
						is_first, is_final,
						&prev_block_used_only_literals);
			cache_ptr = &c->p.n.match_cache[0];
			deflate_near_optimal_save_stats(c);
			deflate_near_optimal_init_stats(c);
			in_block_begin = in_next;
		}
	} while (in_next != in_end && !os->overflow);
}

#endif /* SUPPORT_NEAR_OPTIMAL_PARSING */

#ifdef MATCHFINDER_CONTINUED
#  undef should_end_block
#  undef ready_to_check_block
#endif
