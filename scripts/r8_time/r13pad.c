
/*
 * Test-only (r13 layout experiment): appended to lib/deflate_compress.c by
 * msbuild.bat.  R13_PAD = P1 + 100 * P2, each of P1 and P2 being 0, 16, 32 or
 * 48.  P1 adds an unused function of that many bytes (arm64) whose name sorts
 * right after deflate_find_min_cost_path, so that every later function in the
 * MSVC image moves by P1 bytes while its code stays the same.  P2 does the
 * same after deflate_find_min_cost_path_simd.
 */
#ifdef R13_PAD
#include <intrin.h>
#if R13_PAD % 100 > 0
#pragma comment(linker, "/INCLUDE:deflate_find_min_cost_path_pad")
void deflate_find_min_cost_path_pad(void);
void deflate_find_min_cost_path_pad(void)
{
	__nop(); __nop(); __nop();
#if R13_PAD % 100 >= 32
	__nop(); __nop(); __nop(); __nop();
#endif
#if R13_PAD % 100 >= 48
	__nop(); __nop(); __nop(); __nop();
#endif
}
#endif
#if R13_PAD / 100 > 0
#pragma comment(linker, "/INCLUDE:deflate_find_min_cost_path_simd_pad")
void deflate_find_min_cost_path_simd_pad(void);
void deflate_find_min_cost_path_simd_pad(void)
{
	__nop(); __nop(); __nop();
#if R13_PAD / 100 >= 32
	__nop(); __nop(); __nop(); __nop();
#endif
#if R13_PAD / 100 >= 48
	__nop(); __nop(); __nop(); __nop();
#endif
}
#endif
#endif
