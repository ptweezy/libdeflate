/*
 * decompress_template.h
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
 * This is the actual DEFLATE decompression routine, lifted out of
 * deflate_decompress.c so that it can be compiled multiple times with different
 * target instruction sets.
 */

#ifndef ATTRIBUTES
#  define ATTRIBUTES
#endif
#ifndef EXTRACT_VARBITS
#  define EXTRACT_VARBITS(word, count)	((word) & BITMASK(count))
#endif
#ifndef EXTRACT_VARBITS8
#  define EXTRACT_VARBITS8(word, count)	((word) & BITMASK((u8)(count)))
#endif

#define PAIRS_FASTLOOP_FUNCNAME_(funcname)	funcname##_pairs_fastloop
#define PAIRS_FASTLOOP_FUNCNAME__(funcname)	PAIRS_FASTLOOP_FUNCNAME_(funcname)
#define PAIRS_FASTLOOP_FUNCNAME			PAIRS_FASTLOOP_FUNCNAME__(FUNCNAME)

/*
 * This is the fastloop for litlen decode tables that contain pairs of literals.
 * The main routine below calls it with the state in d->fastloop_state.
 *
 * It is a separate function which isn't inlined, and it takes its state from
 * the decompressor rather than from the caller's stack frame, so that it
 * disturbs the code that the compiler generates for the main routine as little
 * as possible.  The main routine's own fastloop is sensitive to that, as it has
 * more variables than some architectures have registers.
 *
 * On return, d->fastloop_state.block_done is true if the end-of-block symbol
 * was decoded.  Otherwise, the generic loop must be used for what remains of
 * the block.
 */
static ATTRIBUTES NOINLINE MAYBE_UNUSED enum libdeflate_result
PAIRS_FASTLOOP_FUNCNAME(struct libdeflate_decompressor * restrict d,
			const u8 * const in_fastloop_end,
			const u8 * const out,
			const u8 * const out_fastloop_end)
{
	const bitbuf_t litlen_tablemask = BITMASK(d->litlen_tablebits);
	const u8 *in_next = d->fastloop_state.in_next;
	u8 *out_next = d->fastloop_state.out_next;
	bitbuf_t bitbuf = d->fastloop_state.bitbuf;
	bitbuf_t saved_bitbuf;
	u32 bitsleft = d->fastloop_state.bitsleft;
	u32 entry;

	d->fastloop_state.block_done = false;
	REFILL_BITS_IN_FASTLOOP();
	entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
#define FASTLOOP_LITERAL_PAIRS 1
#include "decompress_fastloop.h"
	goto out;

block_done:
	d->fastloop_state.block_done = true;
out:
	d->fastloop_state.in_next = in_next;
	d->fastloop_state.out_next = out_next;
	d->fastloop_state.bitbuf = bitbuf;
	d->fastloop_state.bitsleft = bitsleft;
	return LIBDEFLATE_SUCCESS;
}

static ATTRIBUTES MAYBE_UNUSED enum libdeflate_result
FUNCNAME(struct libdeflate_decompressor * restrict d,
	 const void * restrict in, size_t in_nbytes,
	 void * restrict out, size_t out_nbytes_avail,
	 size_t *actual_in_nbytes_ret, size_t *actual_out_nbytes_ret)
{
	u8 *out_next = out;
	u8 * const out_end = out_next + out_nbytes_avail;
	u8 * const out_fastloop_end =
		out_end - MIN(out_nbytes_avail, FASTLOOP_MAX_BYTES_WRITTEN);

	/* Input bitstream state; see deflate_decompress.c for documentation */
	const u8 *in_next = in;
	const u8 * const in_end = in_next + in_nbytes;
	const u8 * const in_fastloop_end =
		in_end - MIN(in_nbytes, FASTLOOP_MAX_BYTES_READ);
	bitbuf_t bitbuf = 0;
	bitbuf_t saved_bitbuf;
	u32 bitsleft = 0;
	size_t overread_count = 0;

	bool is_final_block;
	unsigned block_type;
	unsigned num_litlen_syms;
	unsigned num_offset_syms;
	bitbuf_t litlen_tablemask;
	u32 entry;

next_block:
	/* Starting to read the next block */
	;

	STATIC_ASSERT(CAN_CONSUME(1 + 2 + 5 + 5 + 4 + 3));
	REFILL_BITS();

	/* BFINAL: 1 bit */
	is_final_block = bitbuf & BITMASK(1);

	/* BTYPE: 2 bits */
	block_type = (bitbuf >> 1) & BITMASK(2);

	if (block_type == DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN) {

		/* Dynamic Huffman block */

		/* The order in which precode lengths are stored */
		static const u8 deflate_precode_lens_permutation[DEFLATE_NUM_PRECODE_SYMS] = {
			16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
		};

		unsigned num_explicit_precode_lens;
		unsigned i;

		/* Read the codeword length counts. */

		STATIC_ASSERT(DEFLATE_NUM_LITLEN_SYMS == 257 + BITMASK(5));
		num_litlen_syms = 257 + ((bitbuf >> 3) & BITMASK(5));

		STATIC_ASSERT(DEFLATE_NUM_OFFSET_SYMS == 1 + BITMASK(5));
		num_offset_syms = 1 + ((bitbuf >> 8) & BITMASK(5));

		STATIC_ASSERT(DEFLATE_NUM_PRECODE_SYMS == 4 + BITMASK(4));
		num_explicit_precode_lens = 4 + ((bitbuf >> 13) & BITMASK(4));

		d->static_codes_loaded = false;

		/*
		 * Read the precode codeword lengths.
		 *
		 * A 64-bit bitbuffer is just one bit too small to hold the
		 * maximum number of precode lens, so to minimize branches we
		 * merge one len with the previous fields.
		 */
		STATIC_ASSERT(DEFLATE_MAX_PRE_CODEWORD_LEN == (1 << 3) - 1);
		if (CAN_CONSUME(3 * (DEFLATE_NUM_PRECODE_SYMS - 1))) {
			d->u.precode_lens[deflate_precode_lens_permutation[0]] =
				(bitbuf >> 17) & BITMASK(3);
			bitbuf >>= 20;
			bitsleft -= 20;
			REFILL_BITS();
			i = 1;
			do {
				d->u.precode_lens[deflate_precode_lens_permutation[i]] =
					bitbuf & BITMASK(3);
				bitbuf >>= 3;
				bitsleft -= 3;
			} while (++i < num_explicit_precode_lens);
		} else {
			bitbuf >>= 17;
			bitsleft -= 17;
			i = 0;
			do {
				if ((u8)bitsleft < 3)
					REFILL_BITS();
				d->u.precode_lens[deflate_precode_lens_permutation[i]] =
					bitbuf & BITMASK(3);
				bitbuf >>= 3;
				bitsleft -= 3;
			} while (++i < num_explicit_precode_lens);
		}
		for (; i < DEFLATE_NUM_PRECODE_SYMS; i++)
			d->u.precode_lens[deflate_precode_lens_permutation[i]] = 0;

		/* Build the decode table for the precode. */
		SAFETY_CHECK(build_precode_decode_table(d));

		/* Decode the litlen and offset codeword lengths. */
		i = 0;
		do {
			unsigned presym;
			u8 rep_val;
			unsigned rep_count;

			if ((u8)bitsleft < DEFLATE_MAX_PRE_CODEWORD_LEN + 7)
				REFILL_BITS();

			/*
			 * The code below assumes that the precode decode table
			 * doesn't have any subtables.
			 */
			STATIC_ASSERT(PRECODE_TABLEBITS == DEFLATE_MAX_PRE_CODEWORD_LEN);

			/* Decode the next precode symbol. */
			entry = d->u.l.precode_decode_table[
				bitbuf & BITMASK(DEFLATE_MAX_PRE_CODEWORD_LEN)];
			bitbuf >>= (u8)entry;
			bitsleft -= entry; /* optimization: subtract full entry */
			presym = entry >> 16;

			if (presym < 16) {
				/* Explicit codeword length */
				d->u.l.lens[i++] = presym;
				continue;
			}

			/* Run-length encoded codeword lengths */

			/*
			 * Note: we don't need to immediately verify that the
			 * repeat count doesn't overflow the number of elements,
			 * since we've sized the lens array to have enough extra
			 * space to allow for the worst-case overrun (138 zeroes
			 * when only 1 length was remaining).
			 *
			 * In the case of the small repeat counts (presyms 16
			 * and 17), it is fastest to always write the maximum
			 * number of entries.  That gets rid of branches that
			 * would otherwise be required.
			 *
			 * It is not just because of the numerical order that
			 * our checks go in the order 'presym < 16', 'presym ==
			 * 16', and 'presym == 17'.  For typical data this is
			 * ordered from most frequent to least frequent case.
			 */
			STATIC_ASSERT(DEFLATE_MAX_LENS_OVERRUN == 138 - 1);

			if (presym == 16) {
				/* Repeat the previous length 3 - 6 times. */
				SAFETY_CHECK(i != 0);
				rep_val = d->u.l.lens[i - 1];
				STATIC_ASSERT(3 + BITMASK(2) == 6);
				rep_count = 3 + (bitbuf & BITMASK(2));
				bitbuf >>= 2;
				bitsleft -= 2;
				d->u.l.lens[i + 0] = rep_val;
				d->u.l.lens[i + 1] = rep_val;
				d->u.l.lens[i + 2] = rep_val;
				d->u.l.lens[i + 3] = rep_val;
				d->u.l.lens[i + 4] = rep_val;
				d->u.l.lens[i + 5] = rep_val;
				i += rep_count;
			} else if (presym == 17) {
				/* Repeat zero 3 - 10 times. */
				STATIC_ASSERT(3 + BITMASK(3) == 10);
				rep_count = 3 + (bitbuf & BITMASK(3));
				bitbuf >>= 3;
				bitsleft -= 3;
				d->u.l.lens[i + 0] = 0;
				d->u.l.lens[i + 1] = 0;
				d->u.l.lens[i + 2] = 0;
				d->u.l.lens[i + 3] = 0;
				d->u.l.lens[i + 4] = 0;
				d->u.l.lens[i + 5] = 0;
				d->u.l.lens[i + 6] = 0;
				d->u.l.lens[i + 7] = 0;
				d->u.l.lens[i + 8] = 0;
				d->u.l.lens[i + 9] = 0;
				i += rep_count;
			} else {
				/* Repeat zero 11 - 138 times. */
				STATIC_ASSERT(11 + BITMASK(7) == 138);
				rep_count = 11 + (bitbuf & BITMASK(7));
				bitbuf >>= 7;
				bitsleft -= 7;
				memset(&d->u.l.lens[i], 0,
				       rep_count * sizeof(d->u.l.lens[i]));
				i += rep_count;
			}
		} while (i < num_litlen_syms + num_offset_syms);

		/* Unnecessary, but check this for consistency with zlib. */
		SAFETY_CHECK(i == num_litlen_syms + num_offset_syms);

		/*
		 * Note how much input remains, for deciding whether to make
		 * pairs of literals in the litlen decode table.
		 */
		d->in_remaining = in_end - in_next;

	} else if (block_type == DEFLATE_BLOCKTYPE_UNCOMPRESSED) {
		u16 len, nlen;

		/*
		 * Uncompressed block: copy 'len' bytes literally from the input
		 * buffer to the output buffer.
		 */

		bitsleft -= 3; /* for BTYPE and BFINAL */

		/*
		 * Align the bitstream to the next byte boundary.  This means
		 * the next byte boundary as if we were reading a byte at a
		 * time.  Therefore, we have to rewind 'in_next' by any bytes
		 * that have been refilled but not actually consumed yet (not
		 * counting overread bytes, which don't increment 'in_next').
		 */
		bitsleft = (u8)bitsleft;
		SAFETY_CHECK(overread_count <= (bitsleft >> 3));
		in_next -= (bitsleft >> 3) - overread_count;
		overread_count = 0;
		bitbuf = 0;
		bitsleft = 0;

		SAFETY_CHECK(in_end - in_next >= 4);
		len = get_unaligned_le16(in_next);
		nlen = get_unaligned_le16(in_next + 2);
		in_next += 4;

		SAFETY_CHECK(len == (u16)~nlen);
		if (unlikely(len > out_end - out_next))
			return LIBDEFLATE_INSUFFICIENT_SPACE;
		SAFETY_CHECK(len <= in_end - in_next);

		memcpy(out_next, in_next, len);
		in_next += len;
		out_next += len;

		goto block_done;

	} else {
		unsigned i;

		SAFETY_CHECK(block_type == DEFLATE_BLOCKTYPE_STATIC_HUFFMAN);

		/*
		 * Static Huffman block: build the decode tables for the static
		 * codes.  Skip doing so if the tables are already set up from
		 * an earlier static block; this speeds up decompression of
		 * degenerate input of many empty or very short static blocks.
		 *
		 * Afterwards, the remainder is the same as decompressing a
		 * dynamic Huffman block.
		 */

		bitbuf >>= 3; /* for BTYPE and BFINAL */
		bitsleft -= 3;

		if (d->static_codes_loaded)
			goto have_decode_tables;

		d->static_codes_loaded = true;

		STATIC_ASSERT(DEFLATE_NUM_LITLEN_SYMS == 288);
		STATIC_ASSERT(DEFLATE_NUM_OFFSET_SYMS == 32);

		for (i = 0; i < 144; i++)
			d->u.l.lens[i] = 8;
		for (; i < 256; i++)
			d->u.l.lens[i] = 9;
		for (; i < 280; i++)
			d->u.l.lens[i] = 7;
		for (; i < 288; i++)
			d->u.l.lens[i] = 8;

		for (; i < 288 + 32; i++)
			d->u.l.lens[i] = 5;

		num_litlen_syms = 288;
		num_offset_syms = 32;

		/* No two literals of the static code fit in the main table. */
		d->in_remaining = 0;
	}

	/* Decompressing a Huffman block (either dynamic or static) */

	SAFETY_CHECK(build_offset_decode_table(d, num_litlen_syms, num_offset_syms));
	SAFETY_CHECK(build_litlen_decode_table(d, num_litlen_syms, num_offset_syms));
have_decode_tables:
	litlen_tablemask = BITMASK(d->litlen_tablebits);

	/*
	 * This is the "fastloop" for decoding literals and matches.  It does
	 * bounds checks on in_next and out_next in the loop conditions so that
	 * additional bounds checks aren't needed inside the loop body.
	 *
	 * To reduce latency, the bitbuffer is refilled and the next litlen
	 * decode table entry is preloaded before each loop iteration.
	 */
	if (in_next >= in_fastloop_end || out_next >= out_fastloop_end)
		goto generic_loop;
	if (unlikely(d->litlen_has_pairs))
		goto pairs_fastloop;
	REFILL_BITS_IN_FASTLOOP();
	entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
#define FASTLOOP_LITERAL_PAIRS 0
#include "decompress_fastloop.h"

	/*
	 * This is the generic loop for decoding literals and matches.  This
	 * handles cases where in_next and out_next are close to the end of
	 * their respective buffers.  Usually this loop isn't performance-
	 * critical, as most time is spent in the fastloop above instead.  We
	 * therefore omit some optimizations here in favor of smaller code.
	 */
generic_loop:
	for (;;) {
		u32 length, offset;
		const u8 *src;
		u8 *dst;

		REFILL_BITS();
		entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
		saved_bitbuf = bitbuf;
		bitbuf >>= (u8)entry;
		bitsleft -= entry;
		if (unlikely(entry & HUFFDEC_SUBTABLE_POINTER)) {
			entry = d->u.litlen_decode_table[(entry >> 16) +
					EXTRACT_VARBITS(bitbuf, (entry >> 8) & 0x3F)];
			saved_bitbuf = bitbuf;
			bitbuf >>= (u8)entry;
			bitsleft -= entry;
		}
		length = entry >> 16;
		if (entry & HUFFDEC_LITERAL) {
			if (unlikely(out_next == out_end))
				return LIBDEFLATE_INSUFFICIENT_SPACE;
			*out_next++ = length;
			continue;
		}
		if (unlikely(entry & HUFFDEC_END_OF_BLOCK))
			goto end_of_block_or_literal_pair;
		length += EXTRACT_VARBITS8(saved_bitbuf, entry) >> (u8)(entry >> 8);
		if (unlikely(length > out_end - out_next))
			return LIBDEFLATE_INSUFFICIENT_SPACE;

		if (!CAN_CONSUME(LENGTH_MAXBITS + OFFSET_MAXBITS))
			REFILL_BITS();
		entry = d->offset_decode_table[bitbuf & BITMASK(OFFSET_TABLEBITS)];
		if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
			bitbuf >>= OFFSET_TABLEBITS;
			bitsleft -= OFFSET_TABLEBITS;
			entry = d->offset_decode_table[(entry >> 16) +
					EXTRACT_VARBITS(bitbuf, (entry >> 8) & 0x3F)];
			if (!CAN_CONSUME(OFFSET_MAXBITS))
				REFILL_BITS();
		}
		offset = entry >> 16;
		offset += EXTRACT_VARBITS8(bitbuf, entry) >> (u8)(entry >> 8);
		bitbuf >>= (u8)entry;
		bitsleft -= entry;

		SAFETY_CHECK(offset <= out_next - (const u8 *)out);
		src = out_next - offset;
		dst = out_next;
		out_next += length;

		STATIC_ASSERT(DEFLATE_MIN_MATCH_LEN == 3);
		*dst++ = *src++;
		*dst++ = *src++;
		do {
			*dst++ = *src++;
		} while (dst < out_next);
	}

end_of_block_or_literal_pair:
	if (unlikely(entry & HUFFDEC_LITERAL_PAIR)) {
		/*
		 * The generic loop found a pair of literals, which has
		 * HUFFDEC_END_OF_BLOCK set so that the loop doesn't need its
		 * own check for pairs.  Output just the first literal, and put
		 * back the bits of the second one.  This way, the generic loop
		 * refills and checks the bitbuffer at exactly the same points
		 * as it would if there were no pairs.
		 */
		const u32 first_len = (entry >> 8) & 0xF;

		if (unlikely(out_next == out_end))
			return LIBDEFLATE_INSUFFICIENT_SPACE;
		*out_next++ = (u8)(entry >> 16);
		bitbuf = saved_bitbuf >> first_len;
		bitsleft += (u8)entry - first_len;
		goto generic_loop;
	}

block_done:
	/* Finished decoding a block */

	if (!is_final_block)
		goto next_block;

	/* That was the last block. */

	bitsleft = (u8)bitsleft;

	/*
	 * If any of the implicit appended zero bytes were consumed (not just
	 * refilled) before hitting end of stream, then the data is bad.
	 */
	SAFETY_CHECK(overread_count <= (bitsleft >> 3));

	/* Optionally return the actual number of bytes consumed. */
	if (actual_in_nbytes_ret) {
		/* Don't count bytes that were refilled but not consumed. */
		in_next -= (bitsleft >> 3) - overread_count;

		*actual_in_nbytes_ret = in_next - (u8 *)in;
	}

	/* Optionally return the actual number of bytes written. */
	if (actual_out_nbytes_ret) {
		*actual_out_nbytes_ret = out_next - (u8 *)out;
	} else {
		if (out_next != out_end)
			return LIBDEFLATE_SHORT_OUTPUT;
	}
	return LIBDEFLATE_SUCCESS;

pairs_fastloop:
	/*
	 * The litlen decode table contains pairs of literals.  Use the fastloop
	 * for that case, which is a separate function.  (This code is at the end
	 * of this function to keep it out of the way of the usual case.)
	 */
	{
		enum libdeflate_result res;

		d->fastloop_state.in_next = in_next;
		d->fastloop_state.out_next = out_next;
		d->fastloop_state.bitbuf = bitbuf;
		d->fastloop_state.bitsleft = bitsleft;
		res = PAIRS_FASTLOOP_FUNCNAME(d, in_fastloop_end, out,
					      out_fastloop_end);
		if (unlikely(res != LIBDEFLATE_SUCCESS))
			return res;
		in_next = d->fastloop_state.in_next;
		out_next = d->fastloop_state.out_next;
		bitbuf = d->fastloop_state.bitbuf;
		bitsleft = d->fastloop_state.bitsleft;
		if (d->fastloop_state.block_done)
			goto block_done;
		goto generic_loop;
	}
}

#undef FUNCNAME
#undef ATTRIBUTES
#undef EXTRACT_VARBITS
#undef EXTRACT_VARBITS8
#undef PAIRS_FASTLOOP_FUNCNAME_
#undef PAIRS_FASTLOOP_FUNCNAME__
#undef PAIRS_FASTLOOP_FUNCNAME
