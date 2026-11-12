/*
 * test_small_inputs.c
 *
 * Copyright 2026 Parker Loflin
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
 * Test the compression of small inputs.  At levels 1 to 9, the compressor
 * handles inputs up to a length limit with the small-input versions of the
 * matchfinders.  So for every input length from 0 to past the largest limit,
 * and at every compression level, test that:
 *   - The compressed data decompresses to the original data.
 *   - The compressor doesn't read beyond the end of the input or write beyond
 *     the end of the output buffer.
 *   - The compressed data doesn't depend on the inputs that the compressor
 *     compressed before.
 */

#include "test_util.h"

/* Longer than the longest input that is compressed as a small input */
#define MAX_LEN		640

static u8 *in_start, *in_end;
static u8 *out_start, *out_end;
static u8 *back_start, *back_end;

static u32 rand_state = 1;

static u32
rand32(void)
{
	rand_state ^= rand_state << 13;
	rand_state ^= rand_state >> 17;
	rand_state ^= rand_state << 5;
	return rand_state;
}

/* Random bytes: no matches, and a new hash value at nearly every position */
static void
generate_random(u8 *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		p[i] = (u8)(rand32() >> 24);
}

/* Words from a small dictionary: many short matches */
static void
generate_words(u8 *p, size_t n)
{
	u8 words[16][8];
	size_t i = 0;

	generate_random(&words[0][0], sizeof(words));
	while (i < n) {
		const u8 *word = words[rand32() % ARRAY_LEN(words)];
		size_t len = 3 + (rand32() % 6);
		size_t j;

		for (j = 0; j < len && i < n; j++)
			p[i++] = word[j];
	}
}

/* Runs of a byte value: long matches */
static void
generate_runs(u8 *p, size_t n)
{
	size_t i = 0;

	while (i < n) {
		const u8 b = (u8)(rand32() >> 24);
		size_t len = 1 + (rand32() % 64);

		while (len-- && i < n)
			p[i++] = b;
	}
}

/*
 * Compress the first @len bytes of @data and check that the compressed data
 * decompresses to them.  The input and the output buffer end at guard pages.
 * Return the size of the compressed data, and its CRC-32 in @crc_ret.
 */
static size_t
compress_and_check(struct libdeflate_compressor *c,
		   struct libdeflate_decompressor *d,
		   const u8 *data, size_t len, u32 *crc_ret)
{
	const size_t out_avail = libdeflate_deflate_compress_bound(c, len);
	u8 * const in = in_end - len;
	u8 * const out = out_end - out_avail;
	u8 * const back = back_end - len;
	size_t csize;
	size_t actual_out_nbytes = 0;

	ASSERT(out_avail <= (size_t)(out_end - out_start));
	memcpy(in, data, len);
	csize = libdeflate_deflate_compress(c, in, len, out, out_avail);
	ASSERT(csize > 0 && csize <= out_avail);

	ASSERT(libdeflate_deflate_decompress(d, out, csize, back, len,
					     &actual_out_nbytes) ==
	       LIBDEFLATE_SUCCESS);
	ASSERT(actual_out_nbytes == len);
	ASSERT(memcmp(back, data, len) == 0);

	*crc_ret = libdeflate_crc32(0, out, csize);
	return csize;
}

static void
test_small_inputs(const u8 *data, struct libdeflate_decompressor *d)
{
	static size_t csizes[MAX_LEN + 1];
	static u32 crcs[MAX_LEN + 1];
	int level;

	for (level = 0; level <= 12; level++) {
		struct libdeflate_compressor *c =
			libdeflate_alloc_compressor(level);
		size_t len;
		u32 crc;

		ASSERT(c != NULL);

		for (len = 0; len <= MAX_LEN; len++)
			csizes[len] = compress_and_check(c, d, data, len,
							 &crcs[len]);
		/*
		 * Compress the same inputs in the opposite order.  The
		 * compressed data must be the same.
		 */
		for (len = MAX_LEN + 1; len-- > 0; ) {
			ASSERT(compress_and_check(c, d, data, len, &crc) ==
			       csizes[len]);
			ASSERT(crc == crcs[len]);
		}
		libdeflate_free_compressor(c);
	}
}

int
tmain(int argc, tchar *argv[])
{
	u8 data[MAX_LEN];
	struct libdeflate_decompressor *d;

	begin_program(argv);

	alloc_guarded_buffer(MAX_LEN, &in_start, &in_end);
	alloc_guarded_buffer(libdeflate_deflate_compress_bound(NULL, MAX_LEN),
			     &out_start, &out_end);
	alloc_guarded_buffer(MAX_LEN, &back_start, &back_end);

	d = libdeflate_alloc_decompressor();
	ASSERT(d != NULL);

	memset(data, 0, sizeof(data));
	test_small_inputs(data, d);

	generate_random(data, sizeof(data));
	test_small_inputs(data, d);

	generate_words(data, sizeof(data));
	test_small_inputs(data, d);

	generate_runs(data, sizeof(data));
	test_small_inputs(data, d);

	libdeflate_free_decompressor(d);
	free_guarded_buffer(in_start, in_end);
	free_guarded_buffer(out_start, out_end);
	free_guarded_buffer(back_start, back_end);
	return 0;
}
