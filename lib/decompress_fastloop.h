/*
 * decompress_fastloop.h
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
 * This is the "fastloop" of the DEFLATE decompression routine, i.e. the loop
 * that decodes most of the literals and matches of a compressed block.  It is
 * lifted out of decompress_template.h so that it can be compiled into each
 * decompression routine twice: once for litlen decode tables that contain only
 * single literals, and once for litlen decode tables that also contain pairs of
 * literals (see make_literal_pairs()).  The two differ only in how the literal
 * entries in the main part of the litlen decode table are recognized and
 * output, as selected by FASTLOOP_LITERAL_PAIRS:
 *
 *   - If !FASTLOOP_LITERAL_PAIRS, there are only HUFFDEC_LITERAL entries.  Each
 *     contains a single literal, which is output as a byte.
 *
 *   - If FASTLOOP_LITERAL_PAIRS, there are HUFFDEC_LITERAL_PAIR entries too.
 *     Both kinds are handled by the same code, without branching on which kind
 *     it is: 2 bytes are stored, and the output pointer is advanced by 1 or 2.
 *     (FASTLOOP_MAX_BYTES_WRITTEN allows for the 1-byte overrun.)
 *
 * Having two copies of the loop means that decoding with tables that contain no
 * pairs of literals is not slowed down at all by the support for pairs.
 *
 * This file is a fragment of a function body.  It uses the variables of the
 * function that it is included into.  On entry, the bitbuffer must have just
 * been refilled, and 'entry' must be the litlen decode table entry for the next
 * item.  It ends when the fastloop can't continue, or it jumps to 'block_done'.
 */

#undef FASTLOOP_IS_LITERAL
#undef FASTLOOP_LITERAL
#undef FASTLOOP_PUT_LITERAL
#if FASTLOOP_LITERAL_PAIRS
#  define FASTLOOP_IS_LITERAL(entry)	\
	((entry) & (HUFFDEC_LITERAL | HUFFDEC_LITERAL_PAIR))
#  define FASTLOOP_LITERAL(entry)	(entry)
/*
 * A pair has its first literal in bits 23-16, and its second literal in bits
 * 30-24 and bit 15.  A single literal is in bits 23-16; then the second byte
 * that gets stored is garbage, which is overwritten later.
 */
#  define FASTLOOP_PUT_LITERAL(lit)					\
	do {								\
		put_unaligned_le16(((lit) >> 16) | ((lit) & 0x8000),	\
				   out_next);				\
		out_next += 1 + (((lit) / HUFFDEC_LITERAL_PAIR) & 1);	\
	} while (0)
#else
#  define FASTLOOP_IS_LITERAL(entry)	((entry) & HUFFDEC_LITERAL)
#  define FASTLOOP_LITERAL(entry)	((entry) >> 16)
#  define FASTLOOP_PUT_LITERAL(lit)	(*out_next++ = (lit))
#endif

	do {
		u32 length, offset, lit;
		const u8 *src;
		u8 *dst;

		/*
		 * Consume the bits for the litlen decode table entry.  Save the
		 * original bitbuf for later, in case the extra match length
		 * bits need to be extracted from it.
		 */
		saved_bitbuf = bitbuf;
		bitbuf >>= (u8)entry;
		bitsleft -= entry; /* optimization: subtract full entry */

		/*
		 * Begin by checking for a "fast" literal, i.e. a literal that
		 * doesn't need a subtable.  If FASTLOOP_LITERAL_PAIRS, this can
		 * actually be a pair of literals; that also applies everywhere
		 * that "fast literal" is mentioned below.
		 */
		if (FASTLOOP_IS_LITERAL(entry)) {
			/*
			 * On 64-bit platforms, we decode up to 2 extra fast
			 * literals in addition to the primary item, as this
			 * increases performance and still leaves enough bits
			 * remaining for what follows.  We could actually do 3,
			 * assuming LITLEN_TABLEBITS=11, but that actually
			 * decreases performance slightly (perhaps by messing
			 * with the branch prediction of the conditional refill
			 * that happens later while decoding the match offset).
			 *
			 * Note: the definitions of FASTLOOP_MAX_BYTES_WRITTEN
			 * and FASTLOOP_MAX_BYTES_READ need to be updated if the
			 * number of extra fast literals decoded here is changed.
			 */
			if (/* enough bits for 2 fast literals + length + offset preload? */
			    CAN_CONSUME_AND_THEN_PRELOAD(2 * LITLEN_TABLEBITS +
							 LENGTH_MAXBITS,
							 OFFSET_TABLEBITS) &&
			    /* enough bits for 2 fast literals + slow literal + litlen preload? */
			    CAN_CONSUME_AND_THEN_PRELOAD(2 * LITLEN_TABLEBITS +
							 DEFLATE_MAX_LITLEN_CODEWORD_LEN,
							 LITLEN_TABLEBITS)) {
				/* 1st extra fast literal */
				lit = FASTLOOP_LITERAL(entry);
				entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
				saved_bitbuf = bitbuf;
				bitbuf >>= (u8)entry;
				bitsleft -= entry;
				FASTLOOP_PUT_LITERAL(lit);
				if (FASTLOOP_IS_LITERAL(entry)) {
					/* 2nd extra fast literal */
					lit = FASTLOOP_LITERAL(entry);
					entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
					saved_bitbuf = bitbuf;
					bitbuf >>= (u8)entry;
					bitsleft -= entry;
					FASTLOOP_PUT_LITERAL(lit);
					if (FASTLOOP_IS_LITERAL(entry)) {
						/*
						 * Another fast literal, but
						 * this one is in lieu of the
						 * primary item, so it doesn't
						 * count as one of the extras.
						 */
						lit = FASTLOOP_LITERAL(entry);
						entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
						REFILL_BITS_IN_FASTLOOP();
						FASTLOOP_PUT_LITERAL(lit);
						continue;
					}
				}
			} else {
				/*
				 * Decode a literal.  While doing so, preload
				 * the next litlen decode table entry and refill
				 * the bitbuffer.  To reduce latency, we've
				 * arranged for there to be enough "preloadable"
				 * bits remaining to do the table preload
				 * independently of the refill.
				 */
				STATIC_ASSERT(CAN_CONSUME_AND_THEN_PRELOAD(
						LITLEN_TABLEBITS, LITLEN_TABLEBITS));
				lit = FASTLOOP_LITERAL(entry);
				entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
				REFILL_BITS_IN_FASTLOOP();
				FASTLOOP_PUT_LITERAL(lit);
				continue;
			}
		}

		/*
		 * It's not a literal entry, so it can be a length entry, a
		 * subtable pointer entry, or an end-of-block entry.  Detect the
		 * two unlikely cases by testing the HUFFDEC_EXCEPTIONAL flag.
		 */
		if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
			/* Subtable pointer or end-of-block entry */

			if (unlikely(entry & HUFFDEC_END_OF_BLOCK))
				goto block_done;

			/*
			 * A subtable is required.  Load and consume the
			 * subtable entry.  The subtable entry can be of any
			 * type: literal, length, or end-of-block.
			 */
			entry = d->u.litlen_decode_table[(entry >> 16) +
				EXTRACT_VARBITS(bitbuf, (entry >> 8) & 0x3F)];
			saved_bitbuf = bitbuf;
			bitbuf >>= (u8)entry;
			bitsleft -= entry;

			/*
			 * 32-bit platforms that use the byte-at-a-time refill
			 * method have to do a refill here for there to always
			 * be enough bits to decode a literal that requires a
			 * subtable, then preload the next litlen decode table
			 * entry; or to decode a match length that requires a
			 * subtable, then preload the offset decode table entry.
			 */
			if (!CAN_CONSUME_AND_THEN_PRELOAD(DEFLATE_MAX_LITLEN_CODEWORD_LEN,
							  LITLEN_TABLEBITS) ||
			    !CAN_CONSUME_AND_THEN_PRELOAD(LENGTH_MAXBITS,
							  OFFSET_TABLEBITS))
				REFILL_BITS_IN_FASTLOOP();
			if (entry & HUFFDEC_LITERAL) {
				/* Decode a literal that required a subtable. */
				lit = entry >> 16;
				entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
				REFILL_BITS_IN_FASTLOOP();
				*out_next++ = lit;
				continue;
			}
			if (unlikely(entry & HUFFDEC_END_OF_BLOCK))
				goto block_done;
			/* Else, it's a length that required a subtable. */
		}

		/*
		 * Decode the match length: the length base value associated
		 * with the litlen symbol (which we extract from the decode
		 * table entry), plus the extra length bits.  We don't need to
		 * consume the extra length bits here, as they were included in
		 * the bits consumed by the entry earlier.  We also don't need
		 * to check for too-long matches here, as this is inside the
		 * fastloop where it's already been verified that the output
		 * buffer has enough space remaining to copy a max-length match.
		 */
		length = entry >> 16;
		length += EXTRACT_VARBITS8(saved_bitbuf, entry) >> (u8)(entry >> 8);

		/*
		 * Decode the match offset.  There are enough "preloadable" bits
		 * remaining to preload the offset decode table entry, but a
		 * refill might be needed before consuming it.
		 */
		STATIC_ASSERT(CAN_CONSUME_AND_THEN_PRELOAD(LENGTH_MAXFASTBITS,
							   OFFSET_TABLEBITS));
		entry = d->offset_decode_table[bitbuf & BITMASK(OFFSET_TABLEBITS)];
		if (CAN_CONSUME_AND_THEN_PRELOAD(OFFSET_MAXBITS,
						 LITLEN_TABLEBITS)) {
			/*
			 * Decoding a match offset on a 64-bit platform.  We may
			 * need to refill once, but then we can decode the whole
			 * offset and preload the next litlen table entry.
			 */
			if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
				/* Offset codeword requires a subtable */
				if (unlikely((u8)bitsleft < OFFSET_MAXBITS +
					     LITLEN_TABLEBITS - PRELOAD_SLACK))
					REFILL_BITS_IN_FASTLOOP();
				bitbuf >>= OFFSET_TABLEBITS;
				bitsleft -= OFFSET_TABLEBITS;
				entry = d->offset_decode_table[(entry >> 16) +
					EXTRACT_VARBITS(bitbuf, (entry >> 8) & 0x3F)];
			} else if (unlikely((u8)bitsleft < OFFSET_MAXFASTBITS +
					    LITLEN_TABLEBITS - PRELOAD_SLACK))
				REFILL_BITS_IN_FASTLOOP();
		} else {
			/* Decoding a match offset on a 32-bit platform */
			REFILL_BITS_IN_FASTLOOP();
			if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
				/* Offset codeword requires a subtable */
				bitbuf >>= OFFSET_TABLEBITS;
				bitsleft -= OFFSET_TABLEBITS;
				entry = d->offset_decode_table[(entry >> 16) +
					EXTRACT_VARBITS(bitbuf, (entry >> 8) & 0x3F)];
				REFILL_BITS_IN_FASTLOOP();
				/* No further refill needed before extra bits */
				STATIC_ASSERT(CAN_CONSUME(
					OFFSET_MAXBITS - OFFSET_TABLEBITS));
			} else {
				/* No refill needed before extra bits */
				STATIC_ASSERT(CAN_CONSUME(OFFSET_MAXFASTBITS));
			}
		}
		saved_bitbuf = bitbuf;
		bitbuf >>= (u8)entry;
		bitsleft -= entry; /* optimization: subtract full entry */
		offset = entry >> 16;
		offset += EXTRACT_VARBITS8(saved_bitbuf, entry) >> (u8)(entry >> 8);

		/* Validate the match offset; needed even in the fastloop. */
		SAFETY_CHECK(offset <= out_next - (const u8 *)out);
		src = out_next - offset;
		dst = out_next;
		out_next += length;

		/*
		 * Before starting to issue the instructions to copy the match,
		 * refill the bitbuffer and preload the litlen decode table
		 * entry for the next loop iteration.  This can increase
		 * performance by allowing the latency of the match copy to
		 * overlap with these other operations.  To further reduce
		 * latency, we've arranged for there to be enough bits remaining
		 * to do the table preload independently of the refill, except
		 * on 32-bit platforms using the byte-at-a-time refill method.
		 */
		if (!CAN_CONSUME_AND_THEN_PRELOAD(
			MAX(OFFSET_MAXBITS - OFFSET_TABLEBITS,
			    OFFSET_MAXFASTBITS),
			LITLEN_TABLEBITS) &&
		    unlikely((u8)bitsleft < LITLEN_TABLEBITS - PRELOAD_SLACK))
			REFILL_BITS_IN_FASTLOOP();
		entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
		REFILL_BITS_IN_FASTLOOP();

		/*
		 * Copy the match.  On most CPUs the fastest method is a
		 * word-at-a-time copy, unconditionally copying about 5 words
		 * since this is enough for most matches without being too much.
		 *
		 * The normal word-at-a-time copy works for offset >= WORDBYTES,
		 * which is most cases.  The case of offset == 1 is also common
		 * and is worth optimizing for, since it is just RLE encoding of
		 * the previous byte, which is the result of compressing long
		 * runs of the same byte.
		 *
		 * Writing past the match 'length' is allowed here, since it's
		 * been ensured there is enough output space left for a slight
		 * overrun.  FASTLOOP_MAX_BYTES_WRITTEN needs to be updated if
		 * the maximum possible overrun here is changed.
		 */
		if (UNALIGNED_ACCESS_IS_FAST && offset >= WORDBYTES) {
			store_word_unaligned(load_word_unaligned(src), dst);
			src += WORDBYTES;
			dst += WORDBYTES;
			store_word_unaligned(load_word_unaligned(src), dst);
			src += WORDBYTES;
			dst += WORDBYTES;
			store_word_unaligned(load_word_unaligned(src), dst);
			src += WORDBYTES;
			dst += WORDBYTES;
			store_word_unaligned(load_word_unaligned(src), dst);
			src += WORDBYTES;
			dst += WORDBYTES;
			store_word_unaligned(load_word_unaligned(src), dst);
			src += WORDBYTES;
			dst += WORDBYTES;
			while (dst < out_next) {
				store_word_unaligned(load_word_unaligned(src), dst);
				src += WORDBYTES;
				dst += WORDBYTES;
				store_word_unaligned(load_word_unaligned(src), dst);
				src += WORDBYTES;
				dst += WORDBYTES;
				store_word_unaligned(load_word_unaligned(src), dst);
				src += WORDBYTES;
				dst += WORDBYTES;
				store_word_unaligned(load_word_unaligned(src), dst);
				src += WORDBYTES;
				dst += WORDBYTES;
				store_word_unaligned(load_word_unaligned(src), dst);
				src += WORDBYTES;
				dst += WORDBYTES;
			}
		} else if (UNALIGNED_ACCESS_IS_FAST && offset == 1) {
			machine_word_t v;

			/*
			 * This part tends to get auto-vectorized, so keep it
			 * copying a multiple of 16 bytes at a time.
			 */
			v = (machine_word_t)0x0101010101010101 * src[0];
			store_word_unaligned(v, dst);
			dst += WORDBYTES;
			store_word_unaligned(v, dst);
			dst += WORDBYTES;
			store_word_unaligned(v, dst);
			dst += WORDBYTES;
			store_word_unaligned(v, dst);
			dst += WORDBYTES;
			while (dst < out_next) {
				store_word_unaligned(v, dst);
				dst += WORDBYTES;
				store_word_unaligned(v, dst);
				dst += WORDBYTES;
				store_word_unaligned(v, dst);
				dst += WORDBYTES;
				store_word_unaligned(v, dst);
				dst += WORDBYTES;
			}
		} else if (UNALIGNED_ACCESS_IS_FAST) {
			store_word_unaligned(load_word_unaligned(src), dst);
			src += offset;
			dst += offset;
			store_word_unaligned(load_word_unaligned(src), dst);
			src += offset;
			dst += offset;
			do {
				store_word_unaligned(load_word_unaligned(src), dst);
				src += offset;
				dst += offset;
				store_word_unaligned(load_word_unaligned(src), dst);
				src += offset;
				dst += offset;
			} while (dst < out_next);
		} else {
			*dst++ = *src++;
			*dst++ = *src++;
			do {
				*dst++ = *src++;
			} while (dst < out_next);
		}
	} while (in_next < in_fastloop_end && out_next < out_fastloop_end);

#undef FASTLOOP_IS_LITERAL
#undef FASTLOOP_LITERAL
#undef FASTLOOP_PUT_LITERAL
#undef FASTLOOP_LITERAL_PAIRS
