/*
 * probe.c: print what kind of platform the identity tools were built for and
 * run on, as seen by libdeflate's own configuration macros.
 */
#include "common_defs.h"
#include <stdio.h>

int main(void)
{
	union {
		unsigned u;
		unsigned char c[sizeof(unsigned)];
	} e;

	e.u = 1;
	printf("pointer_bits=%u size_t_bits=%u WORDBITS=%u\n",
	       (unsigned)(8 * sizeof(void *)), (unsigned)(8 * sizeof(size_t)),
	       (unsigned)WORDBITS);
	printf("runtime_little_endian=%d CPU_IS_LITTLE_ENDIAN=%d\n",
	       e.c[0] == 1, CPU_IS_LITTLE_ENDIAN() ? 1 : 0);
	printf("UNALIGNED_ACCESS_IS_FAST=%d\n", UNALIGNED_ACCESS_IS_FAST);
	printf("arch_macros=%s%s%s%s%s%s%s%s%s\n",
#ifdef ARCH_X86_64
	       "ARCH_X86_64 ",
#else
	       "",
#endif
#ifdef ARCH_X86_32
	       "ARCH_X86_32 ",
#else
	       "",
#endif
#ifdef ARCH_ARM64
	       "ARCH_ARM64 ",
#else
	       "",
#endif
#ifdef ARCH_ARM32
	       "ARCH_ARM32 ",
#else
	       "",
#endif
#ifdef ARCH_RISCV
	       "ARCH_RISCV ",
#else
	       "",
#endif
#ifdef __s390x__
	       "__s390x__ ",
#else
	       "",
#endif
#ifdef __powerpc64__
	       "__powerpc64__ ",
#elif defined(__powerpc__)
	       "__powerpc__ ",
#else
	       "",
#endif
#ifdef __ARM_NEON
	       "__ARM_NEON ",
#else
	       "",
#endif
#ifdef __ARM_FEATURE_UNALIGNED
	       "__ARM_FEATURE_UNALIGNED "
#else
	       ""
#endif
	       );
#if defined(_MSC_VER) && !defined(__clang__)
	printf("compiler=MSVC %d\n", (int)_MSC_FULL_VER);
#elif defined(__VERSION__)
	printf("compiler=%s\n", __VERSION__);
#endif
	printf("probe-ok\n");
	return 0;
}
