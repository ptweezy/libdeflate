/*
 * ldbench.c - low-noise A/B benchmark harness for libdeflate (test-only;
 * the r8_time variant of handoff/tools/bench/ldbench.c).
 *
 * usage: ldbench [-l LEVEL] [-s CHUNK] [-r REPS] [-m c|d|cd] [-H] [-x] FILE...
 *        ldbench -T               print which timers work, then exit
 *        ldbench -K ITERS [-r REPS]   time the positive-control chain
 *
 * Compresses FILE in chunks of CHUNK bytes (0 = whole file) at LEVEL and
 * times REPS repetitions.  Every repetition is measured with three clocks:
 *   wall  CLOCK_MONOTONIC nanoseconds
 *   cpu   CLOCK_THREAD_CPUTIME_ID nanoseconds
 *   cyc   user-mode CPU cycles (perf_event_open), if the machine has them
 *
 * Default output, one line per file (unchanged from the original):
 *   name level ratio% c_min c_med c_mbs d_min d_med d_mbs csize [hash]
 * where c_min etc. are cycles per byte if the cycle counter works and
 * nanoseconds per byte otherwise.
 *
 * -x (compression only) prints instead:
 *   name level wall_min cpu_min cyc_min wall_med csize hash
 * with the minimum over the repetitions of each clock, per input byte
 * (cyc_min is 0 without a cycle counter).  Hosted CI runners are VMs
 * without a virtual PMU, so there the scripts use wall_min.
 *
 * -K ITERS times ITERS iterations of the dependent multiply chain that
 * mkctl.py's posK control adds to libdeflate_deflate_compress(), and prints
 *   calib ITERS wall_min cpu_min cyc_min
 * per iteration.  The scripts use it to predict the control's cost on the
 * machine it runs on, whatever its clock frequency and multiplier latency.
 */
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif
#include <libdeflate.h>
#ifdef __linux__
#  include <linux/perf_event.h>
#  include <sys/ioctl.h>
#  include <sys/syscall.h>
#endif
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/resource.h>

/*
 * Probe mode (-A SET): one line per file with the addresses of the buffers,
 * the stack and the code, the huge-page backing of the buffers, the page
 * faults and six PMU events for every repetition.  SET picks the events.
 */
#define PROBE_NEV 6
static int probe_set = -1;
static int probe_fd[PROBE_NEV] = { -1, -1, -1, -1, -1, -1 };
#if defined(__aarch64__)
/* Arm PMUv3 common events */
static const unsigned probe_events[2][PROBE_NEV] = {
	/* INST_RETIRED, BR_MIS_PRED, L1D_CACHE_REFILL, L1I_CACHE_REFILL,
	 * L2D_CACHE_REFILL, STALL_BACKEND */
	{ 0x08, 0x10, 0x03, 0x01, 0x17, 0x24 },
	/* L1D_TLB_REFILL, L1I_TLB_REFILL, DTLB_WALK, ITLB_WALK,
	 * STALL_FRONTEND, BR_RETIRED */
	{ 0x05, 0x02, 0x34, 0x35, 0x23, 0x21 },
};
#endif

static void probe_init(void)
{
#if defined(__aarch64__) && defined(__linux__)
	for (int i = 0; i < PROBE_NEV; i++) {
		struct perf_event_attr pe;

		memset(&pe, 0, sizeof(pe));
		pe.type = PERF_TYPE_RAW;
		pe.size = sizeof(pe);
		pe.config = probe_events[probe_set & 1][i];
		pe.exclude_kernel = 1;
		pe.exclude_hv = 1;
		probe_fd[i] = syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
	}
#endif
}

static void probe_read(uint64_t *v)
{
	for (int i = 0; i < PROBE_NEV; i++) {
		v[i] = 0;
		if (probe_fd[i] >= 0 &&
		    read(probe_fd[i], &v[i], sizeof(v[i])) != sizeof(v[i]))
			v[i] = 0;
	}
}

/* AnonHugePages (kB) of the mapping that contains p; -1 if not found */
static long probe_huge_kb(const void *p)
{
	FILE *f = fopen("/proc/self/smaps", "r");
	char line[512];
	unsigned long a, b, x = (unsigned long)p;
	int in = 0;
	long kb = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		/* only the lines that start a mapping have "start-end" */
		if (sscanf(line, "%lx-%lx ", &a, &b) == 2) {
			in = x >= a && x < b;
			continue;
		}
		if (in && strncmp(line, "AnonHugePages:", 14) == 0) {
			kb = strtol(line + 14, NULL, 10);
			break;
		}
	}
	fclose(f);
	return kb;
}

static long probe_minflt(void)
{
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_minflt;
}

static int cyc_fd = -1;
static int cyc_errno;

static uint64_t clk(clockid_t id)
{
	struct timespec ts;

	clock_gettime(id, &ts);
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void cyc_init(void)
{
#ifdef __linux__
	struct perf_event_attr pe;

	if (getenv("LDBENCH_NOCYC") != NULL) {
		cyc_errno = ECANCELED;
		return;
	}
	memset(&pe, 0, sizeof(pe));
	pe.type = PERF_TYPE_HARDWARE;
	pe.size = sizeof(pe);
	pe.config = PERF_COUNT_HW_CPU_CYCLES;
	pe.exclude_kernel = 1;
	pe.exclude_hv = 1;
	cyc_fd = syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
	if (cyc_fd < 0)
		cyc_errno = errno;
#else
	cyc_errno = ENOSYS;
#endif
}

static uint64_t cyc_read(void)
{
	uint64_t v = 0;

	if (cyc_fd < 0)
		return 0;
	if (read(cyc_fd, &v, sizeof(v)) != sizeof(v))
		v = 0;
	return v;
}

struct stamp {
	uint64_t wall, cpu, cyc;
};

/* The wall clock is read innermost: it is the one the CI scripts use. */
static void stamp_begin(struct stamp *s)
{
	s->cyc = cyc_read();
	s->cpu = clk(CLOCK_THREAD_CPUTIME_ID);
	s->wall = clk(CLOCK_MONOTONIC);
}

static void stamp_end(struct stamp *s)
{
	uint64_t wall = clk(CLOCK_MONOTONIC);
	uint64_t cpu = clk(CLOCK_THREAD_CPUTIME_ID);
	uint64_t cyc = cyc_read();

	s->wall = wall - s->wall;
	s->cpu = cpu - s->cpu;
	s->cyc = cyc - s->cyc;
}

static int cmpd(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

/* The same statements as mkctl.py's posK control. */
static __attribute__((noinline)) void chain(size_t r8n)
{
	unsigned long r8x = 1;

	while (r8n--)
#if defined(__aarch64__)
		__asm__ volatile("mul %0, %0, %0" : "+r"(r8x));
#elif defined(__x86_64__) || defined(__i386__)
		__asm__ volatile("imul %0, %0" : "+r"(r8x));
#else
		r8x = r8x * 3 + 1;
#endif
	__asm__ volatile("" : : "r"(r8x));
}

static int calibrate(size_t iters, int reps)
{
	double w = 1e99, c = 1e99, y = 1e99;

	for (int r = 0; r < reps; r++) {
		struct stamp s;

		stamp_begin(&s);
		chain(iters);
		stamp_end(&s);
		if ((double)s.wall / iters < w)
			w = (double)s.wall / iters;
		if ((double)s.cpu / iters < c)
			c = (double)s.cpu / iters;
		if ((double)s.cyc / iters < y)
			y = (double)s.cyc / iters;
	}
	printf("calib %zu %.6f %.6f %.6f\n", iters, w, c, y);
	return 0;
}

int main(int argc, char **argv)
{
	int level = 6, reps = 15, opt, print_hash = 0, ext = 0, timers = 0;
	size_t chunk = 0, calib = 0;
	const char *mode = "d";

	while ((opt = getopt(argc, argv, "l:s:r:m:HxTK:A:")) != -1) {
		switch (opt) {
		case 'A': probe_set = atoi(optarg); ext = 1; break;
		case 'l': level = atoi(optarg); break;
		case 's': chunk = strtoull(optarg, NULL, 0); break;
		case 'r': reps = atoi(optarg); break;
		case 'm': mode = optarg; break;
		case 'H': print_hash = 1; break;
		case 'x': ext = 1; break;
		case 'T': timers = 1; break;
		case 'K': calib = strtoull(optarg, NULL, 0); break;
		default: return 2;
		}
	}
	cyc_init();
	if (probe_set >= 0)
		probe_init();
	if (timers) {
		if (cyc_fd >= 0)
			printf("timer cycles\n");
		else
			printf("timer ns perf_errno=%d (%s)\n", cyc_errno,
			       strerror(cyc_errno));
		return 0;
	}
	if (calib)
		return calibrate(calib, reps);
	if (cyc_fd < 0 && !ext && getenv("LDBENCH_QUIET") == NULL)
		fprintf(stderr, "note: no cycle counter; reporting nanoseconds\n");
	if (ext)
		mode = "c";
	for (int fi = optind; fi < argc; fi++) {
		FILE *f = fopen(argv[fi], "rb");
		if (!f) { perror(argv[fi]); return 1; }
		fseek(f, 0, SEEK_END);
		size_t n = ftell(f);
		fseek(f, 0, SEEK_SET);
		unsigned char *in = malloc(n + 1);
		if (fread(in, 1, n, f) != n) { perror("read"); return 1; }
		fclose(f);
		size_t cs = chunk ? chunk : n;
		size_t nchunks = (n + cs - 1) / cs;
		struct libdeflate_compressor *c = libdeflate_alloc_compressor(level);
		struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
		size_t bound = libdeflate_deflate_compress_bound(c, cs);
		unsigned char *cbuf = malloc(nchunks * bound);
		size_t *clen = malloc(nchunks * sizeof(size_t));
		unsigned char *out = malloc(n + 64);
		size_t ctotal = 0;
		double *ct = calloc(reps, sizeof(double));
		double *cc = calloc(reps, sizeof(double));
		double *cu = calloc(reps, sizeof(double));
		double *cy = calloc(reps, sizeof(double));
		double *dt = calloc(reps, sizeof(double));
		double *dc = calloc(reps, sizeof(double));
		int do_c = strchr(mode, 'c') != NULL;
		int do_d = strchr(mode, 'd') != NULL;

		memset(out, 0, n + 64); /* fault in the output buffer */
		if (probe_set >= 0)
			printf("probe %s %d set %d sp %p in %p c %p cbuf %p code %p data %p",
			       strrchr(argv[fi], '/') ? strrchr(argv[fi], '/') + 1 : argv[fi],
			       level, probe_set, (void *)&level, (void *)in, (void *)c,
			       (void *)cbuf, (void *)libdeflate_deflate_compress,
			       (void *)&cyc_fd);
		for (int r = 0; r < (do_c ? reps : 1); r++) {
			struct stamp s;
			uint64_t pv0[PROBE_NEV], pv1[PROBE_NEV];
			long flt0 = 0;

			if (probe_set >= 0) {
				flt0 = probe_minflt();
				probe_read(pv0);
			}
			stamp_begin(&s);
			ctotal = 0;
			for (size_t i = 0; i < nchunks; i++) {
				size_t len = (i == nchunks - 1) ? n - i * cs : cs;
				clen[i] = libdeflate_deflate_compress(c, in + i * cs, len,
								      cbuf + i * bound, bound);
				if (clen[i] == 0) { fprintf(stderr, "compress failed\n"); return 1; }
				ctotal += clen[i];
			}
			stamp_end(&s);
			ct[r] = (double)s.wall / n;
			cu[r] = (double)s.cpu / n;
			cy[r] = (double)s.cyc / n;
			cc[r] = cyc_fd >= 0 ? cy[r] : ct[r];
			if (probe_set >= 0) {
				probe_read(pv1);
				printf(" rep %d cyc %.5f flt %ld ev", r, cy[r],
				       probe_minflt() - flt0);
				for (int i = 0; i < PROBE_NEV; i++)
					printf(" %.5f", (double)(pv1[i] - pv0[i]) / n);
			}
		}
		if (probe_set >= 0) {
			printf(" huge_in %ld huge_c %ld huge_cbuf %ld n %zu csize %zu\n",
			       probe_huge_kb(in), probe_huge_kb(c),
			       probe_huge_kb(cbuf), n, ctotal);
			goto next;
		}
		for (int r = 0; r < (do_d ? reps : 0); r++) {
			struct stamp s;

			stamp_begin(&s);
			for (size_t i = 0; i < nchunks; i++) {
				size_t len = (i == nchunks - 1) ? n - i * cs : cs;
				enum libdeflate_result res =
					libdeflate_deflate_decompress(d, cbuf + i * bound, clen[i],
								      out + i * cs, len, NULL);
				if (res != LIBDEFLATE_SUCCESS) { fprintf(stderr, "decompress failed: %d\n", res); return 1; }
			}
			stamp_end(&s);
			dt[r] = (double)s.wall / n;
			dc[r] = cyc_fd >= 0 ? (double)s.cyc / n : dt[r];
			if (r == 0 && memcmp(in, out, n) != 0) { fprintf(stderr, "MISMATCH\n"); return 1; }
		}
		const char *name = strrchr(argv[fi], '/');
		unsigned long long h = 1469598103934665603ULL;

		name = name ? name + 1 : argv[fi];
		for (size_t i = 0; i < nchunks; i++)
			for (size_t j = 0; j < clen[i]; j++)
				h = (h ^ cbuf[i * bound + j]) * 1099511628211ULL;
		qsort(ct, reps, sizeof(double), cmpd);
		qsort(cc, reps, sizeof(double), cmpd);
		qsort(cu, reps, sizeof(double), cmpd);
		qsort(cy, reps, sizeof(double), cmpd);
		if (ext) {
			printf("%s %d %.5f %.5f %.5f %.5f %zu %016llx\n", name, level,
			       ct[0], cu[0], cy[0], ct[reps / 2], ctotal, h);
			goto next;
		}
		printf("%s %d %.3f", name, level, 100.0 * ctotal / n);
		if (do_c)
			printf(" %.4f %.4f %.1f", cc[0], cc[reps / 2], 1e3 / ct[reps / 2]);
		else
			printf(" - - -");
		if (do_d) {
			qsort(dt, reps, sizeof(double), cmpd);
			qsort(dc, reps, sizeof(double), cmpd);
			printf(" %.4f %.4f %.1f", dc[0], dc[reps / 2], 1e3 / dt[reps / 2]);
		} else {
			printf(" - - -");
		}
		if (print_hash)
			printf(" %zu %016llx\n", ctotal, h);
		else
			printf(" %zu\n", ctotal);
next:
		fflush(stdout);
		libdeflate_free_compressor(c);
		libdeflate_free_decompressor(d);
		free(in); free(cbuf); free(clen); free(out);
		free(ct); free(cc); free(cu); free(cy); free(dt); free(dc);
	}
	return 0;
}
