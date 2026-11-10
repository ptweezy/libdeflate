
/*
 * Test-only (r13 layout experiment): appended to lib/deflate_compress.c by
 * msbuild.bat.  With R13_PAD = 16, 32 or 48 it adds an unused function of
 * that many bytes (arm64) whose name sorts right after
 * deflate_find_min_cost_path, so that every later function in the MSVC image
 * moves by R13_PAD bytes while its code stays the same.
 */
#if defined(R13_PAD) && R13_PAD > 0
#include <intrin.h>
#pragma comment(linker, "/INCLUDE:deflate_find_min_cost_path_pad")
void deflate_find_min_cost_path_pad(void);
void deflate_find_min_cost_path_pad(void)
{
	__nop(); __nop(); __nop();
#if R13_PAD >= 32
	__nop(); __nop(); __nop(); __nop();
#endif
#if R13_PAD >= 48
	__nop(); __nop(); __nop(); __nop();
#endif
}
#endif
