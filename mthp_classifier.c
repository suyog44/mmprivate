// SPDX-License-Identifier: GPL-2.0
/*
 * mthp_classifier.c v3.1  --  mTHP Autopilot userspace daemon (BPF-free)
 *
 * Changes vs v3.0:
 *   - Two-way hinting: MADV_HUGEPAGE for VMAs >= min_hint_kb,
 *     MADV_NOHUGEPAGE for small anon VMAs in [nohuge_min_kb, nohuge_max_kb).
 *   - Hint-cache entries tagged by kind so HUGE and NOHUGE don't collide.
 *   - --setup / --setup-only writes a memory-first THP sysfs configuration.
 *
 * Build / deploy: see Makefile.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/resource.h>

#ifndef __NR_pidfd_open
# define __NR_pidfd_open 434
#endif
#ifndef __NR_process_madvise
# define __NR_process_madvise 440
#endif
#ifndef MADV_HUGEPAGE
# define MADV_HUGEPAGE 14
#endif
#ifndef MADV_NOHUGEPAGE
# define MADV_NOHUGEPAGE 15
#endif
#ifndef MADV_COLLAPSE
# define MADV_COLLAPSE 25
#endif

static inline int sys_pidfd_open(pid_t pid, unsigned int flags)
{ return (int)syscall(__NR_pidfd_open, pid, flags); }

static inline long sys_process_madvise(int pidfd, const struct iovec *iov,
				       size_t vlen, int advice,
				       unsigned long flags)
{ return syscall(__NR_process_madvise, pidfd, iov, vlen, advice, flags); }

/* -- config -------------------------------------------------------------- */
static unsigned long g_scan_interval_ms = 1000;
static unsigned long g_min_hint_kb      = 128;
static unsigned long g_nohuge_min_kb    = 16;
static unsigned long g_nohuge_max_kb    = 64;
static bool g_nohuge_enabled            = true;
static unsigned long g_hint_ttl_sec     = 30;
static bool g_hint_anon  = true;
static bool g_hint_heap  = true;
static bool g_hint_stack = false;
static bool g_tune_khuge = true;
static bool g_collapse   = false;
static bool g_verbose    = false;
static bool g_dry_run    = false;
static bool g_do_setup   = false;
static unsigned long g_report_sec = 10;

/* -- stats --------------------------------------------------------------- */
static uint64_t st_scans, st_vmas_seen, st_vmas_cached, st_vmas_small;
static uint64_t st_vmas_kind, st_vmas_nowrite, st_hints_huge, st_hints_nohuge;
static uint64_t st_hints_ok, st_hints_esrch, st_hints_eperm, st_hints_err;
static uint64_t st_collapse_ok, st_collapse_err;

/* -- hint cache ---------------------------------------------------------- */
#define HINT_TABLE_SIZE 8192
#define HINT_PROBE      8

enum hint_kind { HK_HUGE = 1, HK_NOHUGE = 2 };

struct hint_entry {
	pid_t pid;
	unsigned long start, end;
	uint64_t last_ns;
	uint8_t kind;
};

static struct hint_entry g_hints[HINT_TABLE_SIZE];

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static bool hint_seen(pid_t pid, unsigned long start, unsigned long end,
		      enum hint_kind kind)
{
	uint64_t h = ((uint64_t)pid * 2654435761ULL) ^ start ^ (end << 13)
		     ^ ((uint64_t)kind << 47);
	unsigned base = h & (HINT_TABLE_SIZE - 1);
	uint64_t now = now_ns();
	uint64_t ttl = g_hint_ttl_sec * 1000000000ULL;
	unsigned evict_idx = base;
	uint64_t evict_age = 0;

	for (int i = 0; i < HINT_PROBE; i++) {
		unsigned idx = (base + i) & (HINT_TABLE_SIZE - 1);
		struct hint_entry *e = &g_hints[idx];
		if (e->pid == pid && e->start == start && e->end == end &&
		    e->kind == kind) {
			bool fresh = (now - e->last_ns) < ttl;
			e->last_ns = now;
			return fresh;
		}
		if (e->pid == 0 || (now - e->last_ns) >= ttl) {
			e->pid = pid; e->start = start; e->end = end;
			e->kind = kind; e->last_ns = now;
			return false;
		}
		uint64_t age = now - e->last_ns;
		if (age > evict_age) { evict_age = age; evict_idx = idx; }
	}
	struct hint_entry *e = &g_hints[evict_idx];
	e->pid = pid; e->start = start; e->end = end;
	e->kind = kind; e->last_ns = now;
	return false;
}

/* -- pidfd cache --------------------------------------------------------- */
#define PIDFD_CACHE_SIZE 256
#define PIDFD_PROBE      8
#define PIDFD_EMPTY      0
#define PIDFD_INVALID    (-1)

struct pidfd_entry { pid_t pid; int fd; };
static struct pidfd_entry g_pidfd_cache[PIDFD_CACHE_SIZE];

static void pidfd_cache_init(void)
{
	for (int i = 0; i < PIDFD_CACHE_SIZE; i++) {
		g_pidfd_cache[i].pid = PIDFD_EMPTY;
		g_pidfd_cache[i].fd = PIDFD_INVALID;
	}
}

static int pidfd_cache_get(pid_t pid)
{
	unsigned base = ((unsigned)pid * 2654435761U) & (PIDFD_CACHE_SIZE - 1);
	for (int i = 0; i < PIDFD_PROBE; i++) {
		struct pidfd_entry *e =
			&g_pidfd_cache[(base + i) & (PIDFD_CACHE_SIZE - 1)];
		if (e->pid == pid && e->fd >= 0) return e->fd;
		if (e->pid == PIDFD_EMPTY) break;
	}
	int fd = sys_pidfd_open(pid, 0);
	if (fd < 0) return -1;
	struct pidfd_entry *e = &g_pidfd_cache[base];
	if (e->fd >= 0) close(e->fd);
	e->pid = pid; e->fd = fd;
	return fd;
}

static void pidfd_cache_evict(pid_t pid)
{
	unsigned base = ((unsigned)pid * 2654435761U) & (PIDFD_CACHE_SIZE - 1);
	for (int i = 0; i < PIDFD_PROBE; i++) {
		struct pidfd_entry *e =
			&g_pidfd_cache[(base + i) & (PIDFD_CACHE_SIZE - 1)];
		if (e->pid == pid) {
			if (e->fd >= 0) close(e->fd);
			e->pid = PIDFD_EMPTY; e->fd = PIDFD_INVALID;
			return;
		}
	}
}

static void pidfd_cache_close_all(void)
{
	for (int i = 0; i < PIDFD_CACHE_SIZE; i++)
		if (g_pidfd_cache[i].fd >= 0) close(g_pidfd_cache[i].fd);
}

/* -- /proc/<pid>/maps parsing -------------------------------------------- */
struct vma_info {
	unsigned long start, end;
	char perms[5];
	char kind;  /* a=anon h=heap s=stack f=file o=other */
};

static char classify_vma(unsigned long inode, unsigned int maj, unsigned int min,
			 const char *path)
{
	if (path[0] == '[') {
		if (!strcmp(path, "[heap]")) return 'h';
		if (!strncmp(path, "[stack", 6)) return 's';
		return 'o';
	}
	if (path[0] == 0 || (inode == 0 && maj == 0 && min == 0)) return 'a';
	return 'f';
}

typedef void (*vma_cb_t)(pid_t pid, const struct vma_info *v);

static int scan_pid_maps(pid_t pid, vma_cb_t cb)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		struct vma_info v = {0};
		unsigned long offset, inode;
		unsigned int maj, min;
		char filepath[512] = {0};
		int n = sscanf(line, "%lx-%lx %4s %lx %x:%x %lu %511[^\n]",
			       &v.start, &v.end, v.perms, &offset,
			       &maj, &min, &inode, filepath);
		if (n < 7) continue;
		v.kind = classify_vma(inode, maj, min, filepath);
		cb(pid, &v);
	}
	fclose(f);
	return 0;
}

/* -- madvise dispatch ---------------------------------------------------- */
static int madvise_vma(pid_t pid, const struct vma_info *v, int advice,
		       enum hint_kind kind, const char *label)
{
	if (hint_seen(pid, v->start, v->end, kind)) {
		st_vmas_cached++;
		return 0;
	}
	if (g_verbose)
		fprintf(stderr, "%s pid=%d %lx-%lx (%lu KB) kind=%c%s\n",
			label, pid, v->start, v->end,
			(v->end - v->start) / 1024, v->kind,
			g_dry_run ? " [dry-run]" : "");

	if (kind == HK_HUGE)   st_hints_huge++;
	if (kind == HK_NOHUGE) st_hints_nohuge++;
	if (g_dry_run) return 0;

	int pidfd = pidfd_cache_get(pid);
	if (pidfd < 0) {
		if (errno == ESRCH) st_hints_esrch++;
		else                st_hints_err++;
		return -1;
	}
	struct iovec iov = { .iov_base = (void *)v->start,
			     .iov_len  = v->end - v->start };
	long r = sys_process_madvise(pidfd, &iov, 1, advice, 0);
	if (r >= 0) {
		st_hints_ok++;
		if (g_collapse && kind == HK_HUGE) {
			r = sys_process_madvise(pidfd, &iov, 1,
						MADV_COLLAPSE, 0);
			if (r >= 0) st_collapse_ok++;
			else        st_collapse_err++;
		}
		return 0;
	}
	if (errno == ESRCH || errno == ENOENT) {
		st_hints_esrch++;
		pidfd_cache_evict(pid);
	} else if (errno == EPERM) {
		st_hints_eperm++;
	} else {
		st_hints_err++;
		if (g_verbose)
			fprintf(stderr, "  process_madvise %s: %s\n",
				label, strerror(errno));
	}
	return -1;
}

/* -- hint decision ------------------------------------------------------- */
static void hint_vma(pid_t pid, const struct vma_info *v)
{
	st_vmas_seen++;
	unsigned long size_kb = (v->end - v->start) / 1024;
	if (v->perms[1] != 'w') { st_vmas_nowrite++; return; }
	switch (v->kind) {
	case 'a': if (!g_hint_anon)  { st_vmas_kind++; return; } break;
	case 'h': if (!g_hint_heap)  { st_vmas_kind++; return; } break;
	case 's': if (!g_hint_stack) { st_vmas_kind++; return; } break;
	default:  st_vmas_kind++; return;
	}

	if (size_kb >= g_min_hint_kb) {
		madvise_vma(pid, v, MADV_HUGEPAGE, HK_HUGE, "huge ");
	} else if (g_nohuge_enabled && v->kind == 'a' &&
		   size_kb >= g_nohuge_min_kb && size_kb < g_nohuge_max_kb) {
		madvise_vma(pid, v, MADV_NOHUGEPAGE, HK_NOHUGE, "nohuge");
	} else {
		st_vmas_small++;
	}
}

static void scan_all_pids(void)
{
	DIR *d = opendir("/proc");
	if (!d) return;
	pid_t self = getpid();
	struct dirent *de;
	st_scans++;
	while ((de = readdir(d))) {
		if (!isdigit((unsigned char)de->d_name[0])) continue;
		char *end;
		long pid = strtol(de->d_name, &end, 10);
		if (*end != 0 || pid <= 0 || pid == self) continue;
		scan_pid_maps((pid_t)pid, hint_vma);
	}
	closedir(d);
}

/* -- khugepaged tuning --------------------------------------------------- */
#define KHUGE_BASE "/sys/kernel/mm/transparent_hugepage/khugepaged"
#define MAX_TRACKED_ORDER 10

struct system_info {
	uint64_t buddy_free[MAX_TRACKED_ORDER];
	uint64_t total_pages;
	uint32_t frag_index;
	uint32_t direct_reclaim_active;
};

_Static_assert(sizeof(struct system_info) == 96,
	       "system_info ABI mismatch: update kernel module and daemon together");

struct khuge_tier {
	unsigned int frag_lo, frag_hi;
	unsigned int sleep_ms, pages_to_scan;
};

static const struct khuge_tier khuge_tiers[] = {
	{   0,  30,   500, 8192 },
	{  30,  70,  2000, 4096 },
	{  70, 101,  8000, 2048 },
};

static unsigned int g_last_tier = 0xFFu;

static void write_sysfs_uint(const char *path, unsigned int val)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return;
	char buf[32];
	int n = snprintf(buf, sizeof(buf), "%u\n", val);
	ssize_t r = write(fd, buf, n);
	(void)r;
	close(fd);
}

static void write_sysfs_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return;
	ssize_t r = write(fd, val, strlen(val));
	(void)r;
	close(fd);
}

static void tune_khugepaged(unsigned int frag_index)
{
	unsigned int tier = 1;
	for (unsigned i = 0; i < sizeof(khuge_tiers) / sizeof(khuge_tiers[0]); i++) {
		if (frag_index >= khuge_tiers[i].frag_lo &&
		    frag_index <  khuge_tiers[i].frag_hi) { tier = i; break; }
	}
	if (tier == g_last_tier) return;
	write_sysfs_uint(KHUGE_BASE "/scan_sleep_millisecs",
			 khuge_tiers[tier].sleep_ms);
	write_sysfs_uint(KHUGE_BASE "/pages_to_scan",
			 khuge_tiers[tier].pages_to_scan);
	g_last_tier = tier;
	if (g_verbose)
		fprintf(stderr, "khugepaged: frag=%u tier=%u sleep=%ums scan=%u\n",
			frag_index, tier, khuge_tiers[tier].sleep_ms,
			khuge_tiers[tier].pages_to_scan);
}

static int read_system_info_bin(struct system_info *si)
{
	int fd = open("/sys/kernel/mthp_autopilot/system_info_bin", O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, si, sizeof(*si));
	close(fd);
	return (n == (ssize_t)sizeof(*si)) ? 0 : -1;
}

static int read_system_info_text(struct system_info *si)
{
	int fd = open("/sys/kernel/mthp_autopilot/system_info", O_RDONLY);
	if (fd < 0) return -1;
	char buf[1024];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) return -1;
	buf[n] = 0;
	memset(si, 0, sizeof(*si));
	char *p = strstr(buf, "frag_index:");
	if (!p) return -1;
	return (sscanf(p + 11, " %u", &si->frag_index) == 1) ? 0 : -1;
}

static void bridge_update(void)
{
	if (!g_tune_khuge) return;
	struct system_info si;
	if (read_system_info_bin(&si) == 0 ||
	    read_system_info_text(&si) == 0)
		tune_khugepaged(si.frag_index);
}

/* -- memory-first THP setup ---------------------------------------------- */
static void setup_memory_first(void)
{
	const char *thp = "/sys/kernel/mm/transparent_hugepage";
	char path[512];

	fprintf(stderr, "[setup] configuring THP for memory-first policy\n");

	snprintf(path, sizeof(path), "%s/enabled", thp);
	write_sysfs_str(path, "madvise");

	snprintf(path, sizeof(path), "%s/defrag", thp);
	write_sysfs_str(path, "defer+madvise");

	DIR *d = opendir(thp);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d))) {
			if (strncmp(de->d_name, "hugepages-", 10) != 0)
				continue;
			snprintf(path, sizeof(path), "%s/%s/enabled",
				 thp, de->d_name);
			write_sysfs_str(path, "madvise");
		}
		closedir(d);
	}

	snprintf(path, sizeof(path), "%s/khugepaged/defrag", thp);
	write_sysfs_str(path, "1");

	fprintf(stderr, "[setup] done\n");
}

/* -- signals ------------------------------------------------------------- */
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_dump_stats = 0;
static void sig_stop(int s) { (void)s; g_running = 0; }
static void sig_dump(int s) { (void)s; g_dump_stats = 1; }

static void install_signals(void)
{
	struct sigaction sa = {0};
	sa.sa_handler = sig_stop;
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sa.sa_handler = sig_dump;
	sigaction(SIGUSR1, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

/* -- reporting ----------------------------------------------------------- */
static void print_summary(FILE *out, const char *title)
{
	fprintf(out, "\n=== %s ===\n", title);
	fprintf(out, "  scans                : %" PRIu64 "\n", st_scans);
	fprintf(out, "  vmas seen            : %" PRIu64 "\n", st_vmas_seen);
	fprintf(out, "  MADV_HUGEPAGE issued : %" PRIu64 "\n", st_hints_huge);
	if (g_nohuge_enabled)
		fprintf(out, "  MADV_NOHUGEPAGE      : %" PRIu64 "\n",
			st_hints_nohuge);
	fprintf(out, "    ok                 : %" PRIu64 "\n", st_hints_ok);
	fprintf(out, "    ESRCH (exited)     : %" PRIu64 "\n", st_hints_esrch);
	fprintf(out, "    EPERM              : %" PRIu64 "\n", st_hints_eperm);
	fprintf(out, "    other err          : %" PRIu64 "\n", st_hints_err);
	if (g_collapse) {
		fprintf(out, "  collapse ok          : %" PRIu64 "\n", st_collapse_ok);
		fprintf(out, "  collapse err         : %" PRIu64 "\n", st_collapse_err);
	}
	fprintf(out, "  skipped (cached)     : %" PRIu64 "\n", st_vmas_cached);
	fprintf(out, "  skipped (size)       : %" PRIu64 "\n", st_vmas_small);
	fprintf(out, "  skipped (wrong kind) : %" PRIu64 "\n", st_vmas_kind);
	fprintf(out, "  skipped (RO)         : %" PRIu64 "\n", st_vmas_nowrite);
}

/* -- CLI ----------------------------------------------------------------- */
static void usage(const char *prog)
{
	fprintf(stderr,
"Usage: %s [options]\n"
"\n"
"mTHP Autopilot daemon v3.1. Two-way THP hinting via process_madvise().\n"
"\n"
"Hinting policy:\n"
"  VMA >= --min-kb                              -> MADV_HUGEPAGE\n"
"  anon VMA in [--nohuge-min-kb, --nohuge-max-kb) -> MADV_NOHUGEPAGE\n"
"  other                                        -> ignored\n"
"\n"
"Options:\n"
"  --interval-ms N     scan interval, ms (default: 1000)\n"
"  --min-kb N          MADV_HUGEPAGE threshold, KB (default: 128)\n"
"  --nohuge-min-kb N   MADV_NOHUGEPAGE lower bound, KB (default: 16)\n"
"  --nohuge-max-kb N   MADV_NOHUGEPAGE upper bound, KB (default: 64)\n"
"  --no-nohuge         disable MADV_NOHUGEPAGE path\n"
"  --ttl-sec N         hint-cache TTL, seconds (default: 30)\n"
"  --report-sec N      periodic log, seconds (default: 10, 0 = off)\n"
"  --anon / --no-anon       hint regular anon VMAs (default: on)\n"
"  --heap / --no-heap       hint [heap] (default: on)\n"
"  --stack / --no-stack     hint [stack] (default: off)\n"
"  --khuge / --no-khuge     tune khugepaged (default: on)\n"
"  --collapse               also MADV_COLLAPSE each huge-hinted VMA\n"
"  --setup                  apply memory-first THP sysfs config, then run\n"
"  --setup-only             apply sysfs config and exit\n"
"  --dry-run                count but don't issue madvise\n"
"  -v, --verbose            log each hint\n"
"  -h, --help               this help\n"
"\n"
"Signals: SIGINT/SIGTERM -> summary + exit;  SIGUSR1 -> live stats dump.\n"
"Requires root or CAP_SYS_PTRACE to madvise other processes' VMAs.\n",
		prog);
}

static int parse_args(int argc, char **argv)
{
	bool setup_only = false;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
#define NEED_ARG() do { if (i + 1 >= argc) { fprintf(stderr, "%s needs value\n", a); return 1; } } while (0)
		if (!strcmp(a, "--interval-ms")) { NEED_ARG(); g_scan_interval_ms = strtoul(argv[++i], NULL, 10); }
		else if (!strcmp(a, "--min-kb")) { NEED_ARG(); g_min_hint_kb = strtoul(argv[++i], NULL, 10); }
		else if (!strcmp(a, "--nohuge-min-kb")) { NEED_ARG(); g_nohuge_min_kb = strtoul(argv[++i], NULL, 10); }
		else if (!strcmp(a, "--nohuge-max-kb")) { NEED_ARG(); g_nohuge_max_kb = strtoul(argv[++i], NULL, 10); }
		else if (!strcmp(a, "--no-nohuge")) g_nohuge_enabled = false;
		else if (!strcmp(a, "--ttl-sec")) { NEED_ARG(); g_hint_ttl_sec = strtoul(argv[++i], NULL, 10); }
		else if (!strcmp(a, "--report-sec")) { NEED_ARG(); g_report_sec = strtoul(argv[++i], NULL, 10); }
		else if (!strcmp(a, "--anon"))      g_hint_anon = true;
		else if (!strcmp(a, "--no-anon"))   g_hint_anon = false;
		else if (!strcmp(a, "--heap"))      g_hint_heap = true;
		else if (!strcmp(a, "--no-heap"))   g_hint_heap = false;
		else if (!strcmp(a, "--stack"))     g_hint_stack = true;
		else if (!strcmp(a, "--no-stack"))  g_hint_stack = false;
		else if (!strcmp(a, "--khuge"))     g_tune_khuge = true;
		else if (!strcmp(a, "--no-khuge"))  g_tune_khuge = false;
		else if (!strcmp(a, "--collapse"))  g_collapse = true;
		else if (!strcmp(a, "--setup"))     g_do_setup = true;
		else if (!strcmp(a, "--setup-only")){ g_do_setup = true; setup_only = true; }
		else if (!strcmp(a, "--dry-run"))   g_dry_run = true;
		else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) g_verbose = true;
		else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 2; }
		else { fprintf(stderr, "Unknown argument: %s\n", a); usage(argv[0]); return 1; }
#undef NEED_ARG
	}
	if (g_nohuge_enabled && g_nohuge_min_kb >= g_nohuge_max_kb) {
		fprintf(stderr, "nohuge-min-kb must be < nohuge-max-kb\n"); return 1;
	}
	if (g_nohuge_enabled && g_nohuge_max_kb > g_min_hint_kb) {
		fprintf(stderr, "nohuge-max-kb must be <= min-kb\n"); return 1;
	}
	return setup_only ? 3 : 0;
}

static void print_env_summary(void)
{
	printf("mTHP Autopilot daemon v3.1 (no BPF, two-way hinting)\n");
	printf("  interval=%lums ttl=%lus report=%lus\n",
	       g_scan_interval_ms, g_hint_ttl_sec, g_report_sec);
	printf("  HUGE >= %lu KB", g_min_hint_kb);
	if (g_nohuge_enabled)
		printf(", NOHUGE in [%lu, %lu) KB",
		       g_nohuge_min_kb, g_nohuge_max_kb);
	printf("\n  anon=%d heap=%d stack=%d khuge=%d collapse=%d%s\n",
	       g_hint_anon, g_hint_heap, g_hint_stack, g_tune_khuge,
	       g_collapse, g_dry_run ? "  [DRY RUN]" : "");

	if (access("/sys/kernel/mthp_autopilot/system_info_bin", R_OK) != 0 &&
	    access("/sys/kernel/mthp_autopilot/system_info", R_OK) != 0) {
		fprintf(stderr, "  [warn] mthp_autopilot.ko not loaded; "
				"khugepaged tuning disabled\n");
		g_tune_khuge = false;
	}
	if (access("/sys/kernel/debug/mthp_bestfit_stats", R_OK) != 0 &&
	    access("/proc/sys/vm/mthp_bestfit", F_OK) != 0)
		fprintf(stderr, "  [warn] mthp_bestfit.ko not loaded; hints "
				"work without kernel-side order capping\n");
	if (getuid() != 0)
		fprintf(stderr, "  [warn] not root; process_madvise will EPERM\n");
}

int main(int argc, char **argv)
{
	int rc = parse_args(argc, argv);
	if (rc == 2) return 0;
	if (rc == 1) return 1;

	struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
	setrlimit(RLIMIT_NOFILE, &rl);

	if (g_do_setup)
		setup_memory_first();
	if (rc == 3) return 0;

	install_signals();
	pidfd_cache_init();
	memset(g_hints, 0, sizeof(g_hints));
	print_env_summary();
	printf("Running. Ctrl+C to stop, SIGUSR1 for stats dump.\n\n");

	uint64_t last_report = now_ns();
	while (g_running) {
		bridge_update();
		scan_all_pids();
		if (g_dump_stats) {
			print_summary(stderr, "mTHP Autopilot live stats");
			g_dump_stats = 0;
		}
		uint64_t now = now_ns();
		if (g_report_sec &&
		    now - last_report > g_report_sec * 1000000000ULL) {
			fprintf(stderr,
			    "[%lus] scans=%" PRIu64 " vmas=%" PRIu64
			    " huge=%" PRIu64 " nohuge=%" PRIu64
			    " ok=%" PRIu64 " esrch=%" PRIu64
			    " eperm=%" PRIu64 " err=%" PRIu64 "\n",
			    g_report_sec, st_scans, st_vmas_seen,
			    st_hints_huge, st_hints_nohuge, st_hints_ok,
			    st_hints_esrch, st_hints_eperm, st_hints_err);
			last_report = now;
		}
		struct timespec ts = {
			.tv_sec  = g_scan_interval_ms / 1000,
			.tv_nsec = (long)(g_scan_interval_ms % 1000) * 1000000L,
		};
		nanosleep(&ts, NULL);
	}

	print_summary(stdout, "mTHP Autopilot Summary");
	pidfd_cache_close_all();
	return 0;
}
