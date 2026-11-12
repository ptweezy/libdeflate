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
 *	If 1, instantiate the functions for small inputs.  These take the
 *	additional parameter 'small_base', and they don't require the
 *	matchfinder to have been initialized for the input.  See "Small inputs"
 *	in matchfinder_common.h.
 */

/*
 * SMALL_BASE_PARAM is the additional parameter.  POS() converts an offset from
 * 'in_base' to a position, and INDEX() does the reverse.  NODE_STALE() is
 * whether an entry read from the matchfinder cannot be used; only the version
 * for inputs in general uses 'cutoff'.
 */
#if SMALL_INPUT
#  define SMALL_BASE_PARAM	, const s32 small_base
#  define POS(index)		((index) + small_base)
#  define INDEX(pos)		((pos) - small_base)
#  define NODE_STALE(node)	MATCHFINDER_SMALL_NODE_STALE(node)
#else
#  define SMALL_BASE_PARAM
#  define POS(index)		(index)
#  define INDEX(pos)		(pos)
#  define NODE_STALE(node)	((node) <= cutoff)
#endif

/* See hc_matchfinder_longest_match() in hc_matchfinder.h. */
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
					 SMALL_BASE_PARAM)
{
	u32 depth_remaining = max_search_depth;
	const u8 *best_matchptr = in_next;
	mf_pos_t cur_node3, cur_node4;
	hc_link_t cur_link = 0; /* nodes after cur_node4 (if multi_hop) */
	hc_link_t next_link = 0; /* link of the last node in 'cur_link' */
	unsigned link_remaining = 0; /* number of nodes left in 'cur_link' */
	u32 hash3, hash4;
	u32 next_hashseq;
	u32 seq4;
	const u8 *matchptr;
	u32 len;
	u32 cur_pos = POS(in_next - *in_base_p);
	const u8 *in_base;
	mf_pos_t cutoff MAYBE_UNUSED;

	if (cur_pos == MATCHFINDER_WINDOW_SIZE) {
		hc_matchfinder_slide_window(mf, multi_hop);
		*in_base_p += MATCHFINDER_WINDOW_SIZE;
		cur_pos = 0;
	}

	in_base = *in_base_p;
	cutoff = cur_pos - MATCHFINDER_WINDOW_SIZE;

	if (unlikely(max_len < 5)) /* can we read 4 bytes from 'in_next + 1'? */
		goto out;

	/* Get the precomputed hash codes.  */
	hash3 = next_hashes[0];
	hash4 = next_hashes[1];

	/* From the hash buckets, get the first node of each linked list.  */
	cur_node3 = mf->hash3_tab[hash3];
	cur_node4 = mf->hash4_tab[hash4];

	/* Update for length 3 matches.  This replaces the singleton node in the
	 * 'hash3' bucket with the node for the current sequence.  */
	mf->hash3_tab[hash3] = cur_pos;

	/* Update for length 4 matches.  This prepends the node for the current
	 * sequence to the linked list in the 'hash4' bucket.  */
	mf->hash4_tab[hash4] = cur_pos;
	cur_link = hc_matchfinder_prepend(mf, cur_pos, cur_node4, multi_hop);

	/* Compute the next hash codes.  */
	next_hashseq = get_unaligned_le32(in_next + 1);
	next_hashes[0] = lz_hash(next_hashseq & 0xFFFFFF, HC_MATCHFINDER_HASH3_ORDER);
	next_hashes[1] = lz_hash(next_hashseq, HC_MATCHFINDER_HASH4_ORDER);
	prefetchw(&mf->hash3_tab[next_hashes[0]]);
	prefetchw(&mf->hash4_tab[next_hashes[1]]);

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

		if (NODE_STALE(cur_node3))
			goto out;

		seq4 = load_u32_unaligned(in_next);

		if (best_len < 3) {
			matchptr = &in_base[INDEX(cur_node3)];
			if (load_u24_unaligned(matchptr) == loaded_u32_to_u24(seq4)) {
				best_len = 3;
				best_matchptr = matchptr;
			}
		}

		/* Check for a length 4 match.  */

		if (NODE_STALE(cur_node4))
			goto out;

		HC_BEGIN_WALK();
		for (;;) {
			/* No length 4 match found yet.  Check the first 4 bytes.  */
			matchptr = &in_base[INDEX(cur_node4)];

			if (load_u32_unaligned(matchptr) == seq4)
				break;

			/* The first 4 bytes did not match.  Keep trying.  */
			HC_ADVANCE();
			if (NODE_STALE(cur_node4) || !--depth_remaining)
				goto out;
		}

		/* Found a match of length >= 4.  Extend it to its full length.  */
		best_matchptr = matchptr;
		best_len = lz_extend(in_next, best_matchptr, 4, max_len);
		if (best_len >= nice_len)
			goto out;
		HC_ADVANCE();
		if (NODE_STALE(cur_node4) || !--depth_remaining)
			goto out;
	} else {
		if (NODE_STALE(cur_node4) || best_len >= nice_len)
			goto out;
		HC_BEGIN_WALK();
	}

	/* Check for matches of length >= 5.  */

	for (;;) {
		for (;;) {
			matchptr = &in_base[INDEX(cur_node4)];

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
			if (NODE_STALE(cur_node4) || !--depth_remaining)
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
		if (NODE_STALE(cur_node4) || !--depth_remaining)
			goto out;
	}
#undef HC_BEGIN_WALK
#undef HC_ADVANCE
out:
	*offset_ret = in_next - best_matchptr;
	return best_len;
}

/* See hc_matchfinder_skip_bytes() in hc_matchfinder.h. */
static forceinline void
ADD_SUFFIX(hc_matchfinder_skip_bytes)(struct hc_matchfinder * const mf,
				      const u8 ** const in_base_p,
				      const u8 *in_next,
				      const u8 * const in_end,
				      const u32 count,
				      u32 * const next_hashes,
				      const bool multi_hop
				      SMALL_BASE_PARAM)
{
	u32 cur_pos;
	u32 hash3, hash4;
	u32 next_hashseq;
	u32 remaining = count;

	if (unlikely(count + 5 > in_end - in_next))
		return;

	cur_pos = POS(in_next - *in_base_p);
	hash3 = next_hashes[0];
	hash4 = next_hashes[1];
	do {
		if (cur_pos == MATCHFINDER_WINDOW_SIZE) {
			hc_matchfinder_slide_window(mf, multi_hop);
			*in_base_p += MATCHFINDER_WINDOW_SIZE;
			cur_pos = 0;
		}
		mf->hash3_tab[hash3] = cur_pos;
		hc_matchfinder_prepend(mf, cur_pos, mf->hash4_tab[hash4],
				       multi_hop);
		mf->hash4_tab[hash4] = cur_pos;

		next_hashseq = get_unaligned_le32(++in_next);
		hash3 = lz_hash(next_hashseq & 0xFFFFFF, HC_MATCHFINDER_HASH3_ORDER);
		hash4 = lz_hash(next_hashseq, HC_MATCHFINDER_HASH4_ORDER);
		cur_pos++;
	} while (--remaining);

	prefetchw(&mf->hash3_tab[hash3]);
	prefetchw(&mf->hash4_tab[hash4]);
	next_hashes[0] = hash3;
	next_hashes[1] = hash4;
}

#undef SMALL_BASE_PARAM
#undef POS
#undef INDEX
#undef NODE_STALE

#undef SUFFIX
#undef SMALL_INPUT
