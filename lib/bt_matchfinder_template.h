/*
 * bt_matchfinder_template.h - template for bt_matchfinder_advance_one_byte()
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
 * bt_matchfinder.h includes it twice with different "arguments", so it
 * intentionally has no include guard.
 *
 * It implements bt_matchfinder_advance_one_byte().
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
 * SMALL_BASE_PARAM is the additional parameter.  INDEX() converts a position
 * to an offset from 'in_base'.  NODE_STALE() is whether an entry read from the
 * matchfinder cannot be used, and NODE_VALID() is the opposite; only the
 * versions for inputs in general use 'cutoff'.
 *
 * ROOT_STALE() is NODE_STALE() for the root of a tree.  For a small input, the
 * root must also have the hash of its list, 'hash4'.  The search re-roots the
 * tree, relinking each node it visits, so descending from a leftover root
 * would rearrange the tree of another hash.  An earlier position of the input
 * that has this hash was inserted into this list, so it isn't a leftover, and
 * all the nodes below it were linked during the input.
 */
#if SMALL_INPUT
#  define SMALL_BASE_PARAM	, const s32 small_base
#  define INDEX(pos)		((pos) - small_base)
#  define NODE_STALE(node)	MATCHFINDER_SMALL_NODE_STALE(node)
#  define NODE_VALID(node)	(!MATCHFINDER_SMALL_NODE_STALE(node))
#  define ROOT_STALE(node)						\
	(NODE_STALE(node) ||						\
	 lz_hash(get_unaligned_le32(&in_base[INDEX(node)]),		\
		 BT_MATCHFINDER_HASH4_ORDER) != hash4)
#else
#  define SMALL_BASE_PARAM
#  define INDEX(pos)		(pos)
#  define NODE_STALE(node)	((node) <= cutoff)
#  define NODE_VALID(node)	((node) > cutoff)
#  define ROOT_STALE(node)	NODE_STALE(node)
#endif

/* Advance the binary tree matchfinder by one byte, optionally recording
 * matches.  @record_matches should be a compile-time constant.  */
static forceinline struct lz_match *
ADD_SUFFIX(bt_matchfinder_advance_one_byte)(struct bt_matchfinder * const mf,
					    const u8 * const in_base,
					    const ptrdiff_t cur_pos,
					    const u32 max_len,
					    const u32 nice_len,
					    const u32 max_search_depth,
					    u32 * const next_hashes,
					    struct lz_match *lz_matchptr,
					    const bool record_matches
					    SMALL_BASE_PARAM)
{
	const u8 *in_next = in_base + INDEX(cur_pos);
	u32 depth_remaining = max_search_depth;
	const s32 cutoff MAYBE_UNUSED = cur_pos - MATCHFINDER_WINDOW_SIZE;
	u32 next_hashseq;
	u32 hash3;
	u32 hash4;
	s32 cur_node;
#if BT_MATCHFINDER_HASH3_WAYS >= 2
	s32 cur_node_2;
#endif
	const u8 *matchptr;
	mf_pos_t *pending_lt_ptr, *pending_gt_ptr;
	u32 best_lt_len, best_gt_len;
	u32 len;
	u32 best_len = 3;

	STATIC_ASSERT(BT_MATCHFINDER_HASH3_WAYS >= 1 &&
		      BT_MATCHFINDER_HASH3_WAYS <= 2);

	next_hashseq = get_unaligned_le32(in_next + 1);

	hash3 = next_hashes[0];
	hash4 = next_hashes[1];

	next_hashes[0] = lz_hash(next_hashseq & 0xFFFFFF, BT_MATCHFINDER_HASH3_ORDER);
	next_hashes[1] = lz_hash(next_hashseq, BT_MATCHFINDER_HASH4_ORDER);
	prefetchw(&mf->hash3_tab[next_hashes[0]]);
	prefetchw(&mf->hash4_tab[next_hashes[1]]);

	cur_node = mf->hash3_tab[hash3][0];
	mf->hash3_tab[hash3][0] = cur_pos;
#if BT_MATCHFINDER_HASH3_WAYS >= 2
	cur_node_2 = mf->hash3_tab[hash3][1];
	mf->hash3_tab[hash3][1] = cur_node;
#endif
	if (record_matches && NODE_VALID(cur_node)) {
		u32 seq3 = load_u24_unaligned(in_next);
		if (seq3 == load_u24_unaligned(&in_base[INDEX(cur_node)])) {
			lz_matchptr->length = 3;
			lz_matchptr->offset =
				in_next - &in_base[INDEX(cur_node)];
			lz_matchptr++;
		}
	#if BT_MATCHFINDER_HASH3_WAYS >= 2
		else if (NODE_VALID(cur_node_2) &&
			seq3 == load_u24_unaligned(&in_base[INDEX(cur_node_2)]))
		{
			lz_matchptr->length = 3;
			lz_matchptr->offset =
				in_next - &in_base[INDEX(cur_node_2)];
			lz_matchptr++;
		}
	#endif
	}

	cur_node = mf->hash4_tab[hash4];
	mf->hash4_tab[hash4] = cur_pos;

	pending_lt_ptr = bt_left_child(mf, cur_pos);
	pending_gt_ptr = bt_right_child(mf, cur_pos);

	if (ROOT_STALE(cur_node)) {
		*pending_lt_ptr = MATCHFINDER_INITVAL;
		*pending_gt_ptr = MATCHFINDER_INITVAL;
		return lz_matchptr;
	}

	best_lt_len = 0;
	best_gt_len = 0;
	len = 0;

	for (;;) {
		matchptr = &in_base[INDEX(cur_node)];

		if (matchptr[len] == in_next[len]) {
			len = lz_extend(in_next, matchptr, len + 1, max_len);
			if (!record_matches || len > best_len) {
				if (record_matches) {
					best_len = len;
					lz_matchptr->length = len;
					lz_matchptr->offset = in_next - matchptr;
					lz_matchptr++;
				}
				if (len >= nice_len) {
					*pending_lt_ptr = *bt_left_child(mf, cur_node);
					*pending_gt_ptr = *bt_right_child(mf, cur_node);
					return lz_matchptr;
				}
			}
		}

		if (matchptr[len] < in_next[len]) {
			*pending_lt_ptr = cur_node;
			pending_lt_ptr = bt_right_child(mf, cur_node);
			cur_node = *pending_lt_ptr;
			best_lt_len = len;
			if (best_gt_len < len)
				len = best_gt_len;
		} else {
			*pending_gt_ptr = cur_node;
			pending_gt_ptr = bt_left_child(mf, cur_node);
			cur_node = *pending_gt_ptr;
			best_gt_len = len;
			if (best_lt_len < len)
				len = best_lt_len;
		}

		if (NODE_STALE(cur_node) || !--depth_remaining) {
			*pending_lt_ptr = MATCHFINDER_INITVAL;
			*pending_gt_ptr = MATCHFINDER_INITVAL;
			return lz_matchptr;
		}
	}
}

#undef SMALL_BASE_PARAM
#undef INDEX
#undef NODE_STALE
#undef NODE_VALID
#undef ROOT_STALE

#undef SUFFIX
#undef SMALL_INPUT
