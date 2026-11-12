/*
 * hc_matchfinder_template.h - template for the hc_matchfinder search functions
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
 * hc_matchfinder.h includes it twice with different "arguments", so it
 * intentionally has no include guard.
 *
 * It implements hc_matchfinder_longest_match() and hc_matchfinder_skip_bytes().
 *
 * SUFFIX:
 *	Name suffix to append to all instantiated functions.
 * SMALL_INPUT:
 *	If 0, instantiate the functions for inputs in general.
 *	If 1, instantiate the functions for small inputs, which use a map in
 *	place of each hash table; see "Small inputs" in matchfinder_common.h.
 */

/*
 * SMALL_ORDER_PARAM is the additional parameter of the functions for small
 * inputs: the order of the maps.  BUCKET3 and BUCKET4 are the buckets of
 * 'hash3' and 'hash4'.
 */
#if SMALL_INPUT
#  define SMALL_ORDER_PARAM	, const unsigned order
#  define BUCKET3		(*bucket3)
#  define BUCKET4		(*bucket4)
#else
#  define SMALL_ORDER_PARAM
#  define BUCKET3		mf->hash3_tab[hash3]
#  define BUCKET4		mf->hash4_tab[hash4]
#endif

/*
 * Find the longest match longer than 'best_len' bytes.
 *
 * @mf
 *	The matchfinder structure.
 * @in_base_p
 *	Location of a pointer which points to the place in the input data the
 *	matchfinder currently stores positions relative to.  This may be updated
 *	by this function.
 * @in_next
 *	Pointer to the next position in the input buffer, i.e. the sequence
 *	being matched against.
 * @best_len
 *	Require a match longer than this length.
 * @max_len
 *	The maximum permissible match length at this position.
 * @nice_len
 *	Stop searching if a match of at least this length is found.
 *	Must be <= @max_len.
 * @max_search_depth
 *	Limit on the number of potential matches to consider.  Must be >= 1.
 * @next_hashes
 *	The precomputed hash codes for the sequence beginning at @in_next.
 *	These will be used and then updated with the precomputed hashcodes for
 *	the sequence beginning at @in_next + 1.
 * @offset_ret
 *	If a match is found, its offset is returned in this location.
 * @multi_hop
 *	Whether to use multi-hop links; see 'next_tab'.
 *
 * Return the length of the match found, or 'best_len' if no match longer than
 * 'best_len' was found.
 */
static forceinline u32
ADD_SUFFIX(hc_matchfinder_longest_match)(struct hc_matchfinder * const mf,
					 const u8 ** const in_base_p,
					 const u8 * const in_next,
					 u32 best_len,
					 const u32 max_len,
					 const u32 nice_len,
					 const u32 max_search_depth,
					 u32 * const next_hashes,
					 u32 * const offset_ret,
					 const bool multi_hop
					 SMALL_ORDER_PARAM)
{
	u32 depth_remaining = max_search_depth;
	const u8 *best_matchptr = in_next;
	mf_pos_t cur_node3, cur_node4;
#if SMALL_INPUT
	mf_pos_t *bucket3, *bucket4;
#endif
	hc_link_t cur_link = 0; /* nodes after cur_node4 (if multi_hop) */
	hc_link_t next_link = 0; /* link of the last node in 'cur_link' */
	unsigned link_remaining = 0; /* number of nodes left in 'cur_link' */
	u32 hash3, hash4;
	u32 next_hashseq;
	u32 seq4;
	const u8 *matchptr;
	u32 len;
	u32 cur_pos = in_next - *in_base_p;
	const u8 *in_base;
	mf_pos_t cutoff;

#if !SMALL_INPUT
	if (cur_pos == MATCHFINDER_WINDOW_SIZE) {
		hc_matchfinder_slide_window(mf, multi_hop);
		*in_base_p += MATCHFINDER_WINDOW_SIZE;
		cur_pos = 0;
	}
#endif

	in_base = *in_base_p;
	cutoff = cur_pos - MATCHFINDER_WINDOW_SIZE;

	if (unlikely(max_len < 5)) /* can we read 4 bytes from 'in_next + 1'? */
		goto out;

	/* Get the precomputed hash codes.  */
	hash3 = next_hashes[0];
	hash4 = next_hashes[1];

	/* From the hash buckets, get the first node of each linked list.  */
#if SMALL_INPUT
	bucket3 = matchfinder_small_bucket(mf->hash3_tab,
					   HC_MATCHFINDER_SLOT_SIZE, hash3,
					   HC_MATCHFINDER_HASH3_ORDER, order);
	bucket4 = matchfinder_small_bucket(mf->hash4_tab,
					   HC_MATCHFINDER_SLOT_SIZE, hash4,
					   HC_MATCHFINDER_HASH4_ORDER, order);
#endif
	cur_node3 = BUCKET3;
	cur_node4 = BUCKET4;

	/* Update for length 3 matches.  This replaces the singleton node in the
	 * 'hash3' bucket with the node for the current sequence.  */
	BUCKET3 = cur_pos;

	/* Update for length 4 matches.  This prepends the node for the current
	 * sequence to the linked list in the 'hash4' bucket.  */
	BUCKET4 = cur_pos;
	cur_link = hc_matchfinder_prepend(mf, cur_pos, cur_node4, multi_hop);

	/* Compute the next hash codes.  */
	next_hashseq = get_unaligned_le32(in_next + 1);
	next_hashes[0] = lz_hash(next_hashseq & 0xFFFFFF, HC_MATCHFINDER_HASH3_ORDER);
	next_hashes[1] = lz_hash(next_hashseq, HC_MATCHFINDER_HASH4_ORDER);
#if !SMALL_INPUT
	prefetchw(&mf->hash3_tab[next_hashes[0]]);
	prefetchw(&mf->hash4_tab[next_hashes[1]]);
#endif

/*
 * Prepare to walk the linked list.  'cur_link' contains the nodes that follow
 * the first node.  Already start loading the link of the last of them, so that
 * it will hopefully be available by the time it's needed.
 */
#define HC_BEGIN_WALK()							\
	do {								\
		if (multi_hop) {					\
			link_remaining = HC_MATCHFINDER_HOPS_PER_WORD;	\
			next_link = hc_matchfinder_get_link(mf,		\
						hc_last_hop(cur_link));	\
		}							\
	} while (0)

/* Advance to the next node in the linked list.  */
#define HC_ADVANCE()							\
	do {								\
		if (!multi_hop) {					\
			cur_node4 = mf->next_tab.single_hop[cur_node4 &	\
					(MATCHFINDER_WINDOW_SIZE - 1)];	\
		} else {						\
			cur_node4 = (mf_pos_t)cur_link;			\
			if (--link_remaining != 0) {			\
				cur_link >>= 16;			\
			} else {					\
				/* cur_node4 is the last node of the	\
				 * old 'cur_link', so 'next_link' is	\
				 * its link.  Use it, and start loading	\
				 * the link of its last node.  */	\
				cur_link = next_link;			\
				link_remaining =			\
					HC_MATCHFINDER_HOPS_PER_WORD;	\
				next_link = hc_matchfinder_get_link(mf,	\
						hc_last_hop(cur_link));	\
			}						\
		}							\
	} while (0)

	if (best_len < 4) {  /* No match of length >= 4 found yet?  */

		/* Check for a length 3 match if needed.  */

		if (cur_node3 <= cutoff)
			goto out;

		seq4 = load_u32_unaligned(in_next);

		if (best_len < 3) {
			matchptr = &in_base[cur_node3];
			if (load_u24_unaligned(matchptr) == loaded_u32_to_u24(seq4)) {
				best_len = 3;
				best_matchptr = matchptr;
			}
		}

		/* Check for a length 4 match.  */

		if (cur_node4 <= cutoff)
			goto out;

		HC_BEGIN_WALK();
		for (;;) {
			/* No length 4 match found yet.  Check the first 4 bytes.  */
			matchptr = &in_base[cur_node4];

			if (load_u32_unaligned(matchptr) == seq4)
				break;

			/* The first 4 bytes did not match.  Keep trying.  */
			HC_ADVANCE();
			if (cur_node4 <= cutoff || !--depth_remaining)
				goto out;
		}

		/* Found a match of length >= 4.  Extend it to its full length.  */
		best_matchptr = matchptr;
		best_len = lz_extend(in_next, best_matchptr, 4, max_len);
		if (best_len >= nice_len)
			goto out;
		HC_ADVANCE();
		if (cur_node4 <= cutoff || !--depth_remaining)
			goto out;
	} else {
		if (cur_node4 <= cutoff || best_len >= nice_len)
			goto out;
		HC_BEGIN_WALK();
	}

	/* Check for matches of length >= 5.  */

	for (;;) {
		for (;;) {
			matchptr = &in_base[cur_node4];

			/* Already found a length 4 match.  Try for a longer
			 * match; start by checking either the last 4 bytes and
			 * the first 4 bytes, or the last byte.  (The last byte,
			 * the one which would extend the match length by 1, is
			 * the most important.)  */
		#if UNALIGNED_ACCESS_IS_FAST
			if ((load_u32_unaligned(matchptr + best_len - 3) ==
			     load_u32_unaligned(in_next + best_len - 3)) &&
			    (load_u32_unaligned(matchptr) ==
			     load_u32_unaligned(in_next)))
		#else
			if (matchptr[best_len] == in_next[best_len])
		#endif
				break;

			/* Continue to the next node in the list.  */
			HC_ADVANCE();
			if (cur_node4 <= cutoff || !--depth_remaining)
				goto out;
		}

	#if UNALIGNED_ACCESS_IS_FAST
		len = 4;
	#else
		len = 0;
	#endif
		len = lz_extend(in_next, matchptr, len, max_len);
		if (len > best_len) {
			/* This is the new longest match.  */
			best_len = len;
			best_matchptr = matchptr;
			if (best_len >= nice_len)
				goto out;
		}

		/* Continue to the next node in the list.  */
		HC_ADVANCE();
		if (cur_node4 <= cutoff || !--depth_remaining)
			goto out;
	}
#undef HC_BEGIN_WALK
#undef HC_ADVANCE
out:
	*offset_ret = in_next - best_matchptr;
	return best_len;
}

/*
 * Advance the matchfinder, but don't search for matches.
 *
 * @mf
 *	The matchfinder structure.
 * @in_base_p
 *	Location of a pointer which points to the place in the input data the
 *	matchfinder currently stores positions relative to.  This may be updated
 *	by this function.
 * @in_next
 *	Pointer to the next position in the input buffer.
 * @in_end
 *	Pointer to the end of the input buffer.
 * @count
 *	The number of bytes to advance.  Must be > 0.
 * @next_hashes
 *	The precomputed hash codes for the sequence beginning at @in_next.
 *	These will be used and then updated with the precomputed hashcodes for
 *	the sequence beginning at @in_next + @count.
 * @multi_hop
 *	Whether to use multi-hop links; see 'next_tab'.
 */
static forceinline void
ADD_SUFFIX(hc_matchfinder_skip_bytes)(struct hc_matchfinder * const mf,
				      const u8 ** const in_base_p,
				      const u8 *in_next,
				      const u8 * const in_end,
				      const u32 count,
				      u32 * const next_hashes,
				      const bool multi_hop
				      SMALL_ORDER_PARAM)
{
	u32 cur_pos;
	u32 hash3, hash4;
	u32 next_hashseq;
	u32 remaining = count;
#if SMALL_INPUT
	mf_pos_t *bucket3, *bucket4;
#endif

	if (unlikely(count + 5 > in_end - in_next))
		return;

	cur_pos = in_next - *in_base_p;
	hash3 = next_hashes[0];
	hash4 = next_hashes[1];
	do {
#if SMALL_INPUT
		bucket3 = matchfinder_small_bucket(mf->hash3_tab,
						   HC_MATCHFINDER_SLOT_SIZE, hash3,
						   HC_MATCHFINDER_HASH3_ORDER, order);
		bucket4 = matchfinder_small_bucket(mf->hash4_tab,
						   HC_MATCHFINDER_SLOT_SIZE, hash4,
						   HC_MATCHFINDER_HASH4_ORDER, order);
#else
		if (cur_pos == MATCHFINDER_WINDOW_SIZE) {
			hc_matchfinder_slide_window(mf, multi_hop);
			*in_base_p += MATCHFINDER_WINDOW_SIZE;
			cur_pos = 0;
		}
#endif
		BUCKET3 = cur_pos;
		hc_matchfinder_prepend(mf, cur_pos, BUCKET4, multi_hop);
		BUCKET4 = cur_pos;

		next_hashseq = get_unaligned_le32(++in_next);
		hash3 = lz_hash(next_hashseq & 0xFFFFFF, HC_MATCHFINDER_HASH3_ORDER);
		hash4 = lz_hash(next_hashseq, HC_MATCHFINDER_HASH4_ORDER);
		cur_pos++;
	} while (--remaining);

#if !SMALL_INPUT
	prefetchw(&mf->hash3_tab[hash3]);
	prefetchw(&mf->hash4_tab[hash4]);
#endif
	next_hashes[0] = hash3;
	next_hashes[1] = hash4;
}

#undef SMALL_ORDER_PARAM
#undef BUCKET3
#undef BUCKET4

#undef SUFFIX
#undef SMALL_INPUT
