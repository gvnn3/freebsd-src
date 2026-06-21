/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Benchmark for the ULE scheduler cache-miss-aware priority adjustment.
 *
 * This program forks one cache-hostile worker per available CPU core
 * and a single cache-friendly worker.  Each hostile child is pinned to
 * its own core via cpuset(2); the friendly child floats freely.
 *
 * The hostile workload does random access over a 64 MB array (exceeds LLC),
 * while the friendly workload does sequential access over a 32 KB array
 * (fits in L1).  Both run for a configurable duration.
 *
 * Two phases are run:
 *   Phase 1: cachemiss feature DISABLED (baseline)
 *   Phase 2: cachemiss feature ENABLED
 *
 * Expected result: when the feature is enabled, the cache-friendly
 * workload completes more iterations relative to the hostile workloads.
 *
 * Usage:
 *   cachemiss_bench [-d duration] [-t threshold] [-w weight]
 *
 *   -d duration    Duration of each phase in seconds (default: 5)
 *   -t threshold   Value for kern.sched.ule.cachemiss_threshold (default: 1000)
 *   -w weight      Value for kern.sched.ule.cachemiss_weight (default: 1)
 *
 * Must be run as root (to modify sysctls).
 * Requires a kernel built with HWPMC_HOOKS and the hwpmc module loaded.
 */

#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/cpuset.h>

#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Sysctl paths.
 */
#define	SYSCTL_ENABLED		"kern.sched.ule.cachemiss_enabled"
#define	SYSCTL_WEIGHT		"kern.sched.ule.cachemiss_weight"
#define	SYSCTL_THRESHOLD	"kern.sched.ule.cachemiss_threshold"
#define	SYSCTL_INTERVAL		"kern.sched.ule.cachemiss_interval"

/*
 * Array sizes.
 */
#define	HOSTILE_ARRAY_SIZE	(64 * 1024 * 1024 / sizeof(uint64_t))
#define	FRIENDLY_ARRAY_SIZE	(32 * 1024 / sizeof(uint64_t))

#define	MAX_CPUS		256

/*
 * Data sent from each child worker back to the parent via pipe.
 */
struct worker_report {
	uint64_t	iterations;
	uint64_t	elapsed_usec;
};

static volatile int stop_flag;

/*
 * Pin the calling process to CPU 'cpu' using cpuset_setaffinity(2).
 */
static void
pin_to_cpu(int cpu)
{
	cpuset_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1,
	    sizeof(mask), &mask) == -1)
		err(1, "cpuset_setaffinity(%d)", cpu);
}

static void
sigalrm_handler(int sig __unused)
{

	stop_flag = 1;
}

static int
sysctl_write_int(const char *name, int val)
{

	return (sysctlbyname(name, NULL, NULL, &val, sizeof(val)));
}

static int
sysctl_write_u64(const char *name, uint64_t val)
{

	return (sysctlbyname(name, NULL, NULL, &val, sizeof(val)));
}

static int
sysctl_read_int(const char *name, int *val)
{
	size_t sz = sizeof(*val);

	return (sysctlbyname(name, val, &sz, NULL, 0));
}

static int
sysctl_exists(const char *name)
{
	size_t sz = 0;

	if (sysctlbyname(name, NULL, &sz, NULL, 0) == -1 &&
	    errno == ENOENT)
		return (-1);
	return (0);
}

static int
get_ncpus(void)
{
	int ncpus;
	size_t sz = sizeof(ncpus);

	if (sysctlbyname("hw.ncpu", &ncpus, &sz, NULL, 0) == -1)
		err(1, "sysctl hw.ncpu");
	if (ncpus > MAX_CPUS)
		ncpus = MAX_CPUS;
	return (ncpus);
}

/*
 * Get the user priority of a process.
 */
static int
get_priority(pid_t pid, unsigned char *pri_user)
{
	struct kinfo_proc kp;
	int mib[4];
	size_t sz;

	mib[0] = CTL_KERN;
	mib[1] = KERN_PROC;
	mib[2] = KERN_PROC_PID;
	mib[3] = pid;
	sz = sizeof(kp);
	if (sysctl(mib, 4, &kp, &sz, NULL, 0) == -1)
		return (-1);
	if (pri_user != NULL)
		*pri_user = kp.ki_pri.pri_user;
	return (0);
}

/*
 * Cache-hostile workload: random access over a large array.
 * Returns the number of iterations completed before the alarm fires.
 */
static uint64_t
workload_hostile(int duration_sec)
{
	uint64_t *arr;
	size_t n = HOSTILE_ARRAY_SIZE;
	uint64_t iters = 0;
	unsigned long seed = 67890;
	volatile uint64_t sink;

	arr = malloc(n * sizeof(uint64_t));
	if (arr == NULL)
		err(1, "malloc hostile array");
	for (size_t i = 0; i < n; i++)
		arr[i] = i;

	stop_flag = 0;
	signal(SIGALRM, sigalrm_handler);
	alarm(duration_sec);

	while (!stop_flag) {
		for (int i = 0; i < 1000 && !stop_flag; i++) {
			seed = seed * 6364136223846793005ULL + 1;
			sink = arr[seed % n];
		}
		iters++;
	}

	(void)sink;
	free(arr);
	return (iters);
}

/*
 * Cache-friendly workload: sequential access over a small array.
 * Returns the number of iterations completed before the alarm fires.
 */
static uint64_t
workload_friendly(int duration_sec)
{
	uint64_t *arr;
	size_t n = FRIENDLY_ARRAY_SIZE;
	uint64_t iters = 0;
	uint64_t sum = 0;

	arr = malloc(n * sizeof(uint64_t));
	if (arr == NULL)
		err(1, "malloc friendly array");
	for (size_t i = 0; i < n; i++)
		arr[i] = i;

	stop_flag = 0;
	signal(SIGALRM, sigalrm_handler);
	alarm(duration_sec);

	while (!stop_flag) {
		for (size_t i = 0; i < n && !stop_flag; i++)
			sum += arr[i];
		iters++;
	}

	volatile uint64_t sink = sum;
	(void)sink;
	free(arr);
	return (iters);
}

struct phase_result {
	int		ncpus;
	uint64_t	hostile_iters[MAX_CPUS];
	uint64_t	hostile_usec[MAX_CPUS];
	unsigned char	hostile_pri[MAX_CPUS];
	uint64_t	hostile_total_iters;
	unsigned char	hostile_worst_pri;
	uint64_t	friendly_iters;
	uint64_t	friendly_usec;
	unsigned char	friendly_pri;
};

/*
 * Run one phase of the benchmark.
 *
 * Forks ncpus hostile children (one pinned per CPU) and one unpinned
 * friendly child.  All run for 'duration' seconds.
 */
static void
run_phase(int ncpus, int duration, struct phase_result *res)
{
	int hostile_pipes[MAX_CPUS][2];
	pid_t hostile_pids[MAX_CPUS];
	int friendly_pipe[2];
	pid_t friendly_pid;
	int status;

	memset(res, 0, sizeof(*res));
	res->ncpus = ncpus;

	/* Create pipes for all hostile children. */
	for (int i = 0; i < ncpus; i++) {
		if (pipe(hostile_pipes[i]) == -1)
			err(1, "pipe hostile[%d]", i);
	}
	if (pipe(friendly_pipe) == -1)
		err(1, "pipe friendly");

	/* Fork one hostile child per CPU, pinned. */
	for (int i = 0; i < ncpus; i++) {
		hostile_pids[i] = fork();
		if (hostile_pids[i] == -1)
			err(1, "fork hostile[%d]", i);
		if (hostile_pids[i] == 0) {
			struct worker_report rpt;
			struct timeval t0, t1;

			/* Close all pipe ends we don't need. */
			for (int j = 0; j < ncpus; j++) {
				close(hostile_pipes[j][0]);
				if (j != i)
					close(hostile_pipes[j][1]);
			}
			close(friendly_pipe[0]);
			close(friendly_pipe[1]);

			pin_to_cpu(i);
			gettimeofday(&t0, NULL);
			rpt.iterations = workload_hostile(duration);
			gettimeofday(&t1, NULL);
			rpt.elapsed_usec =
			    (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000 +
			    (uint64_t)(t1.tv_usec - t0.tv_usec);
			write(hostile_pipes[i][1], &rpt, sizeof(rpt));
			close(hostile_pipes[i][1]);
			_exit(0);
		}
	}

	/* Fork one friendly child, not pinned. */
	friendly_pid = fork();
	if (friendly_pid == -1)
		err(1, "fork friendly");
	if (friendly_pid == 0) {
		struct worker_report rpt;
		struct timeval t0, t1;

		for (int j = 0; j < ncpus; j++) {
			close(hostile_pipes[j][0]);
			close(hostile_pipes[j][1]);
		}
		close(friendly_pipe[0]);

		gettimeofday(&t0, NULL);
		rpt.iterations = workload_friendly(duration);
		gettimeofday(&t1, NULL);
		rpt.elapsed_usec =
		    (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000 +
		    (uint64_t)(t1.tv_usec - t0.tv_usec);
		write(friendly_pipe[1], &rpt, sizeof(rpt));
		close(friendly_pipe[1]);
		_exit(0);
	}

	/* Parent: close write ends. */
	for (int i = 0; i < ncpus; i++)
		close(hostile_pipes[i][1]);
	close(friendly_pipe[1]);

	/* Sample priorities near the end of the run. */
	sleep(duration - 1);
	res->hostile_worst_pri = 0;
	res->friendly_pri = 255;
	for (int s = 0; s < 5; s++) {
		for (int i = 0; i < ncpus; i++) {
			unsigned char hp;
			if (get_priority(hostile_pids[i], &hp) == 0) {
				if (hp > res->hostile_pri[i])
					res->hostile_pri[i] = hp;
				if (hp > res->hostile_worst_pri)
					res->hostile_worst_pri = hp;
			}
		}
		{
			unsigned char fp;
			if (get_priority(friendly_pid, &fp) == 0) {
				if (fp < res->friendly_pri)
					res->friendly_pri = fp;
			}
		}
		usleep(100000);
	}

	/* Read worker reports. */
	res->hostile_total_iters = 0;
	for (int i = 0; i < ncpus; i++) {
		struct worker_report rpt;

		memset(&rpt, 0, sizeof(rpt));
		if (read(hostile_pipes[i][0], &rpt, sizeof(rpt)) ==
		    (ssize_t)sizeof(rpt)) {
			res->hostile_iters[i] = rpt.iterations;
			res->hostile_usec[i] = rpt.elapsed_usec;
			res->hostile_total_iters += rpt.iterations;
		}
		close(hostile_pipes[i][0]);
	}

	{
		struct worker_report rpt;

		memset(&rpt, 0, sizeof(rpt));
		if (read(friendly_pipe[0], &rpt, sizeof(rpt)) ==
		    (ssize_t)sizeof(rpt)) {
			res->friendly_iters = rpt.iterations;
			res->friendly_usec = rpt.elapsed_usec;
		}
	}
	close(friendly_pipe[0]);

	/* Reap all children. */
	for (int i = 0; i < ncpus; i++)
		waitpid(hostile_pids[i], &status, 0);
	waitpid(friendly_pid, &status, 0);
}

static void
print_phase(const char *label, const struct phase_result *res)
{

	printf("--- %s ---\n", label);
	for (int i = 0; i < res->ncpus; i++) {
		printf("  Hostile CPU %d:  %ju iterations, %ju.%03ju s, "
		    "pri_user=%u\n",
		    i,
		    (uintmax_t)res->hostile_iters[i],
		    (uintmax_t)(res->hostile_usec[i] / 1000000),
		    (uintmax_t)(res->hostile_usec[i] % 1000000 / 1000),
		    res->hostile_pri[i]);
	}
	printf("  Hostile total:  %ju iterations, worst pri_user=%u\n",
	    (uintmax_t)res->hostile_total_iters,
	    res->hostile_worst_pri);
	printf("  Friendly:       %ju iterations, %ju.%03ju s, pri_user=%u\n",
	    (uintmax_t)res->friendly_iters,
	    (uintmax_t)(res->friendly_usec / 1000000),
	    (uintmax_t)(res->friendly_usec % 1000000 / 1000),
	    res->friendly_pri);
	if (res->hostile_total_iters > 0) {
		printf("  Ratio (friendly/hostile_total): %.2f\n",
		    (double)res->friendly_iters /
		    (double)res->hostile_total_iters);
	}
	printf("\n");
}

static void
usage(void)
{

	fprintf(stderr, "Usage: cachemiss_bench [-d duration] "
	    "[-t threshold] [-w weight]\n");
	exit(1);
}

int
main(int argc, char *argv[])
{
	int duration = 5;
	uint64_t threshold = 1000;
	int weight = 1;
	int ch, ncpus;
	struct phase_result disabled_res, enabled_res;
	int saved_enabled, saved_weight, saved_interval;
	uint64_t saved_threshold;
	size_t sz;

	while ((ch = getopt(argc, argv, "d:t:w:")) != -1) {
		switch (ch) {
		case 'd':
			duration = atoi(optarg);
			if (duration < 2)
				duration = 2;
			break;
		case 't':
			threshold = strtoull(optarg, NULL, 10);
			break;
		case 'w':
			weight = atoi(optarg);
			break;
		default:
			usage();
		}
	}

	if (geteuid() != 0)
		errx(1, "must be run as root");

	if (sysctl_exists(SYSCTL_ENABLED) != 0)
		errx(1, "cachemiss sysctl knobs not found; "
		    "kernel may not have HWPMC_HOOKS");

	ncpus = get_ncpus();

	/* Save current settings. */
	sz = sizeof(saved_enabled);
	sysctlbyname(SYSCTL_ENABLED, &saved_enabled, &sz, NULL, 0);
	sz = sizeof(saved_weight);
	sysctlbyname(SYSCTL_WEIGHT, &saved_weight, &sz, NULL, 0);
	sz = sizeof(saved_threshold);
	sysctlbyname(SYSCTL_THRESHOLD, &saved_threshold, &sz, NULL, 0);
	sz = sizeof(saved_interval);
	sysctlbyname(SYSCTL_INTERVAL, &saved_interval, &sz, NULL, 0);

	printf("=== ULE Cache-Miss Priority Adjustment Benchmark ===\n");
	printf("CPUs: %d (%d hostile children pinned, 1 friendly unpinned)\n",
	    ncpus, ncpus);
	printf("Duration per phase: %d seconds\n", duration);
	printf("Weight: %d, Threshold: %ju\n", weight, (uintmax_t)threshold);
	printf("\n");

	/* ---- Phase 1: Feature DISABLED ---- */
	sysctl_write_int(SYSCTL_ENABLED, 0);
	run_phase(ncpus, duration, &disabled_res);
	print_phase("Phase 1: Feature DISABLED", &disabled_res);

	/* ---- Phase 2: Feature ENABLED ---- */
	sysctl_write_int(SYSCTL_ENABLED, 1);
	sysctl_write_int(SYSCTL_WEIGHT, weight);
	sysctl_write_u64(SYSCTL_THRESHOLD, threshold);

	/* Verify it actually enabled. */
	int enabled_check;
	sysctl_read_int(SYSCTL_ENABLED, &enabled_check);
	if (enabled_check != 1) {
		printf("WARNING: Could not enable cachemiss feature "
		    "(hwpmc may not be loaded).\n");
		printf("Skipping enabled phase.\n");
		goto done;
	}

	run_phase(ncpus, duration, &enabled_res);
	print_phase("Phase 2: Feature ENABLED", &enabled_res);

	/* ---- Summary ---- */
	printf("=== Summary ===\n");
	printf("Priority difference (hostile worst - friendly best):\n");
	printf("  Disabled: %d\n",
	    (int)disabled_res.hostile_worst_pri -
	    (int)disabled_res.friendly_pri);
	printf("  Enabled:  %d\n",
	    (int)enabled_res.hostile_worst_pri -
	    (int)enabled_res.friendly_pri);

	if (disabled_res.hostile_total_iters > 0 &&
	    enabled_res.hostile_total_iters > 0) {
		double ratio_disabled =
		    (double)disabled_res.friendly_iters /
		    (double)disabled_res.hostile_total_iters;
		double ratio_enabled =
		    (double)enabled_res.friendly_iters /
		    (double)enabled_res.hostile_total_iters;
		printf("Iteration ratio change: %.2f -> %.2f\n",
		    ratio_disabled, ratio_enabled);
		if (ratio_enabled > ratio_disabled) {
			printf("RESULT: Cache-friendly workload gained "
			    "%.1f%% more relative CPU time with feature "
			    "enabled.\n",
			    (ratio_enabled / ratio_disabled - 1.0) * 100.0);
		} else {
			printf("RESULT: No improvement observed.  The "
			    "workloads may not be generating enough LLC "
			    "misses, or the system may be lightly loaded.\n");
		}
	}

done:
	/* Restore original settings. */
	sysctl_write_int(SYSCTL_ENABLED, saved_enabled);
	sysctl_write_int(SYSCTL_WEIGHT, saved_weight);
	sysctl_write_u64(SYSCTL_THRESHOLD, saved_threshold);
	sysctl_write_int(SYSCTL_INTERVAL, saved_interval);

	return (0);
}
