/*
 * mspad.c - test-only: linked between msbench.obj and the libdeflate static
 * library by msbuildcm.bat (MSB_PADS), so that all of libdeflate's code moves
 * by PAD bytes while staying the same.  mspad() is exactly PAD bytes long:
 * PAD / NOP_BYTES - 1 nops and the return.
 */
#include <intrin.h>

#if defined(_M_IX86)
#  pragma comment(linker, "/INCLUDE:_mspad")
#else
#  pragma comment(linker, "/INCLUDE:mspad")
#endif

#if defined(_M_ARM64)
#  define NOP_BYTES	4
#else
#  define NOP_BYTES	1
#endif
#define N_NOPS		(PAD / NOP_BYTES - 1)

#define R1	__nop();
#define R2	R1 R1
#define R4	R2 R2
#define R8	R4 R4
#define R16	R8 R8
#define R32	R16 R16

void mspad(void);
void mspad(void)
{
#if N_NOPS & 32
	R32
#endif
#if N_NOPS & 16
	R16
#endif
#if N_NOPS & 8
	R8
#endif
#if N_NOPS & 4
	R4
#endif
#if N_NOPS & 2
	R2
#endif
#if N_NOPS & 1
	R1
#endif
}
