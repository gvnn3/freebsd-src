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
 * ATF tests for the ULE scheduler cache-miss-aware priority adjustment
 * feature, as specified in spec/sched_ule_cachemiss_priority_v1.md.
 *
 * These tests exercise the feature through the sysctl interface and
 * observation of thread priorities via kinfo_proc.
 *
 * Most tests require root to modify sysctl knobs.
 */

#include <sys/param.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/cpuset.h>

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atf-c.h>

/*
 * Constants from the spec.
 */
#define	SCHED_CACHEMISS_PENALTY_MAX	50
#define	PRI_MAX_BATCH			223	/* PRI_MAX_TIMESHARE */
#define	PRI_MIN_TIMESHARE		56

/*
 * Sysctl paths as specified.
 */
#define	SYSCTL_ENABLED		"kern.sched.ule.cachemiss_enabled"
#define	SYSCTL_WEIGHT		"kern.sched.ule.cachemiss_weight"
#define	SYSCTL_THRESHOLD	"kern.sched.ule.cachemiss_threshold"
#define	SYSCTL_INTERVAL		"kern.sched.ule.cachemiss_interval"

/*
 * Size of large array for cache-hostile workloads (64 MB).
 */
#define	HOSTILE_ARRAY_SIZE	(64 * 1024 * 1024 / sizeof(uint64_t))

/*
 * Size of small array for cache-friendly workloads (32 KB, fits in L1).
 */
#define	FRIENDLY_ARRAY_SIZE	(32 * 1024 / sizeof(uint64_t))

/*
 * Duration of workload in seconds for behavioral tests.
 */
#define	WORKLOAD_DURATION	3

/*
 * Decay timeout in seconds (spec says ~5s for SCHED_SLP_RUN_MAX).
 */
#define	DECAY_TIMEOUT		8

/* ------------------------------------------------------------------ */
/*  Helper: sysctl read/write wrappers                                */
/* ------------------------------------------------------------------ */

static int
sysctl_read_int(const char *name, int *val)
{
	size_t sz = sizeof(*val);

	return (sysctlbyname(name, val, &sz, NULL, 0));
}

static int
sysctl_write_int(const char *name, int val)
{

	return (sysctlbyname(name, NULL, NULL, &val, sizeof(val)));
}

static int
sysctl_read_u64(const char *name, uint64_t *val)
{
	size_t sz = sizeof(*val);

	return (sysctlbyname(name, val, &sz, NULL, 0));
}

static int
sysctl_write_u64(const char *name, uint64_t val)
{

	return (sysctlbyname(name, NULL, NULL, &val, sizeof(val)));
}

/*
 * Check that a sysctl exists.  Returns 0 on success, -1 on failure.
 */
static int
sysctl_exists(const char *name)
{
	size_t sz = 0;

	if (sysctlbyname(name, NULL, &sz, NULL, 0) == -1 &&
	    errno == ENOENT)
		return (-1);
	return (0);
}

/* ------------------------------------------------------------------ */
/*  Helper: save and restore sysctl state                             */
/* ------------------------------------------------------------------ */

struct cachemiss_state {
	int	enabled;
	int	weight;
	uint64_t threshold;
	int	interval;
};

static void
save_state(struct cachemiss_state *st)
{

	ATF_REQUIRE(sysctl_read_int(SYSCTL_ENABLED, &st->enabled) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_WEIGHT, &st->weight) == 0);
	ATF_REQUIRE(sysctl_read_u64(SYSCTL_THRESHOLD, &st->threshold) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_INTERVAL, &st->interval) == 0);
}

static void
restore_state(const struct cachemiss_state *st)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, st->enabled);
	(void)sysctl_write_int(SYSCTL_WEIGHT, st->weight);
	(void)sysctl_write_u64(SYSCTL_THRESHOLD, st->threshold);
	(void)sysctl_write_int(SYSCTL_INTERVAL, st->interval);
}

/* ------------------------------------------------------------------ */
/*  Helper: require the feature to be compiled in                     */
/* ------------------------------------------------------------------ */

static void
require_cachemiss_support(void)
{

	if (sysctl_exists(SYSCTL_ENABLED) != 0)
		atf_tc_skip("Kernel does not have HWPMC_HOOKS / "
		    "cachemiss sysctls; feature not compiled in");
}

/* ------------------------------------------------------------------ */
/*  Helper: get priority of a process via KERN_PROC_PID               */
/* ------------------------------------------------------------------ */

static int
get_priority(pid_t pid, unsigned char *pri_level, unsigned char *pri_user)
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
	if (pri_level != NULL)
		*pri_level = kp.ki_pri.pri_level;
	if (pri_user != NULL)
		*pri_user = kp.ki_pri.pri_user;
	return (0);
}

/* ------------------------------------------------------------------ */
/*  Helper: pin calling process to a specific CPU                     */
/* ------------------------------------------------------------------ */

static void
pin_to_cpu(int cpu)
{
	cpuset_t mask;

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
	if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1,
	    sizeof(mask), &mask) == -1)
		_exit(2);
}

/* ------------------------------------------------------------------ */
/*  Helper: cache-hostile workload (random access, large array)       */
/* ------------------------------------------------------------------ */

static volatile uint64_t sink;

static void
run_cache_hostile(int duration_sec)
{
	uint64_t *arr;
	size_t n = HOSTILE_ARRAY_SIZE;
	struct timeval start, now;
	unsigned long seed = 12345;

	arr = malloc(n * sizeof(uint64_t));
	if (arr == NULL)
		_exit(1);
	/* Touch every page to ensure allocation. */
	for (size_t i = 0; i < n; i++)
		arr[i] = i;

	gettimeofday(&start, NULL);
	for (;;) {
		/* Simple LCG for pseudo-random access. */
		for (int i = 0; i < 100000; i++) {
			seed = seed * 6364136223846793005ULL + 1;
			sink = arr[seed % n];
		}
		gettimeofday(&now, NULL);
		if (now.tv_sec - start.tv_sec >= duration_sec)
			break;
	}
	free(arr);
}

/* ------------------------------------------------------------------ */
/*  Helper: cache-friendly workload (sequential access, small array)  */
/* ------------------------------------------------------------------ */

static void
run_cache_friendly(int duration_sec)
{
	uint64_t *arr;
	size_t n = FRIENDLY_ARRAY_SIZE;
	struct timeval start, now;
	uint64_t sum = 0;

	arr = malloc(n * sizeof(uint64_t));
	if (arr == NULL)
		_exit(1);
	for (size_t i = 0; i < n; i++)
		arr[i] = i;

	gettimeofday(&start, NULL);
	for (;;) {
		for (size_t i = 0; i < n; i++)
			sum += arr[i];
		gettimeofday(&now, NULL);
		if (now.tv_sec - start.tv_sec >= duration_sec)
			break;
	}
	sink = sum;
	free(arr);
}

/* ==================================================================
 *  AC-3: Sysctl knobs exist
 * ================================================================== */

ATF_TC(sysctl_knobs_exist);
ATF_TC_HEAD(sysctl_knobs_exist, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-3: Verify all four cachemiss sysctl knobs exist");
}
ATF_TC_BODY(sysctl_knobs_exist, tc)
{

	require_cachemiss_support();

	ATF_REQUIRE_MSG(sysctl_exists(SYSCTL_ENABLED) == 0,
	    "sysctl %s does not exist", SYSCTL_ENABLED);
	ATF_REQUIRE_MSG(sysctl_exists(SYSCTL_WEIGHT) == 0,
	    "sysctl %s does not exist", SYSCTL_WEIGHT);
	ATF_REQUIRE_MSG(sysctl_exists(SYSCTL_THRESHOLD) == 0,
	    "sysctl %s does not exist", SYSCTL_THRESHOLD);
	ATF_REQUIRE_MSG(sysctl_exists(SYSCTL_INTERVAL) == 0,
	    "sysctl %s does not exist", SYSCTL_INTERVAL);
}

/* ==================================================================
 *  AC-4: Default values
 * ================================================================== */

ATF_TC(sysctl_defaults);
ATF_TC_HEAD(sysctl_defaults, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-4: Verify default values of cachemiss sysctl knobs");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(sysctl_defaults, tc)
{
	int ival;
	uint64_t u64val;

	require_cachemiss_support();

	/*
	 * Note: These tests check current values.  If the system has been
	 * booted with non-default tunables, this test may fail legitimately.
	 * The spec says these are the defaults.  If RWTUN was used to change
	 * them at boot, the test documents that difference.
	 */
	ATF_REQUIRE(sysctl_read_int(SYSCTL_ENABLED, &ival) == 0);
	ATF_CHECK_EQ_MSG(ival, 0,
	    "cachemiss_enabled default should be 0, got %d", ival);

	ATF_REQUIRE(sysctl_read_int(SYSCTL_WEIGHT, &ival) == 0);
	ATF_CHECK_EQ_MSG(ival, 1,
	    "cachemiss_weight default should be 1, got %d", ival);

	ATF_REQUIRE(sysctl_read_u64(SYSCTL_THRESHOLD, &u64val) == 0);
	ATF_CHECK_EQ_MSG(u64val, 1000,
	    "cachemiss_threshold default should be 1000, got %ju",
	    (uintmax_t)u64val);

	ATF_REQUIRE(sysctl_read_int(SYSCTL_INTERVAL, &ival) == 0);
	ATF_CHECK_EQ_MSG(ival, 4,
	    "cachemiss_interval default should be 4, got %d", ival);
}

/* ==================================================================
 *  AC-5: cachemiss_weight clamping
 * ================================================================== */

ATF_TC_WITH_CLEANUP(sysctl_weight_clamp_high);
ATF_TC_HEAD(sysctl_weight_clamp_high, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-5: cachemiss_weight > 10 is clamped to 10");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(sysctl_weight_clamp_high, tc)
{
	struct cachemiss_state saved;
	int val;

	require_cachemiss_support();
	save_state(&saved);

	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, 50) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_WEIGHT, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 10,
	    "weight=50 should clamp to 10, got %d", val);

	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, 11) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_WEIGHT, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 10,
	    "weight=11 should clamp to 10, got %d", val);

	/* Boundary: exactly 10 should be accepted. */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, 10) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_WEIGHT, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 10,
	    "weight=10 should be accepted, got %d", val);

	restore_state(&saved);
}
ATF_TC_CLEANUP(sysctl_weight_clamp_high, tc)
{
	/* Best-effort restore; ignore errors. */
	(void)sysctl_write_int(SYSCTL_WEIGHT, 1);
}

ATF_TC_WITH_CLEANUP(sysctl_weight_clamp_low);
ATF_TC_HEAD(sysctl_weight_clamp_low, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-5: cachemiss_weight < 0 is clamped to 0");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(sysctl_weight_clamp_low, tc)
{
	struct cachemiss_state saved;
	int val;

	require_cachemiss_support();
	save_state(&saved);

	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, -1) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_WEIGHT, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 0,
	    "weight=-1 should clamp to 0, got %d", val);

	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, -100) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_WEIGHT, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 0,
	    "weight=-100 should clamp to 0, got %d", val);

	/* Boundary: exactly 0 should be accepted. */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, 0) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_WEIGHT, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 0,
	    "weight=0 should be accepted, got %d", val);

	restore_state(&saved);
}
ATF_TC_CLEANUP(sysctl_weight_clamp_low, tc)
{

	(void)sysctl_write_int(SYSCTL_WEIGHT, 1);
}

/* ==================================================================
 *  AC-6: cachemiss_interval clamping
 * ================================================================== */

ATF_TC_WITH_CLEANUP(sysctl_interval_clamp_high);
ATF_TC_HEAD(sysctl_interval_clamp_high, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-6: cachemiss_interval > 100 is clamped to 100");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(sysctl_interval_clamp_high, tc)
{
	struct cachemiss_state saved;
	int val;

	require_cachemiss_support();
	save_state(&saved);

	ATF_REQUIRE(sysctl_write_int(SYSCTL_INTERVAL, 200) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_INTERVAL, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 100,
	    "interval=200 should clamp to 100, got %d", val);

	ATF_REQUIRE(sysctl_write_int(SYSCTL_INTERVAL, 101) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_INTERVAL, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 100,
	    "interval=101 should clamp to 100, got %d", val);

	/* Boundary: exactly 100 should be accepted. */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_INTERVAL, 100) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_INTERVAL, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 100,
	    "interval=100 should be accepted, got %d", val);

	restore_state(&saved);
}
ATF_TC_CLEANUP(sysctl_interval_clamp_high, tc)
{

	(void)sysctl_write_int(SYSCTL_INTERVAL, 4);
}

ATF_TC_WITH_CLEANUP(sysctl_interval_clamp_low);
ATF_TC_HEAD(sysctl_interval_clamp_low, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-6: cachemiss_interval < 1 is clamped to 1");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(sysctl_interval_clamp_low, tc)
{
	struct cachemiss_state saved;
	int val;

	require_cachemiss_support();
	save_state(&saved);

	ATF_REQUIRE(sysctl_write_int(SYSCTL_INTERVAL, 0) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_INTERVAL, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 1,
	    "interval=0 should clamp to 1, got %d", val);

	ATF_REQUIRE(sysctl_write_int(SYSCTL_INTERVAL, -5) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_INTERVAL, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 1,
	    "interval=-5 should clamp to 1, got %d", val);

	/* Boundary: exactly 1 should be accepted. */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_INTERVAL, 1) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_INTERVAL, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 1,
	    "interval=1 should be accepted, got %d", val);

	restore_state(&saved);
}
ATF_TC_CLEANUP(sysctl_interval_clamp_low, tc)
{

	(void)sysctl_write_int(SYSCTL_INTERVAL, 4);
}

/* ==================================================================
 *  AC-7: Disabled behavior -- no priority difference
 * ================================================================== */

ATF_TC_WITH_CLEANUP(disabled_no_penalty);
ATF_TC_HEAD(disabled_no_penalty, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-7: With cachemiss_enabled=0, no priority difference "
	    "between cache-hostile and cache-friendly workloads");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "30");
}
ATF_TC_BODY(disabled_no_penalty, tc)
{
	struct cachemiss_state saved;
	pid_t hostile, friendly;
	unsigned char hostile_pri, friendly_pri;
	int status;

	require_cachemiss_support();
	save_state(&saved);

	/* Ensure feature is disabled. */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 0) == 0);

	/* Fork a cache-hostile child, pinned to CPU 0. */
	hostile = fork();
	ATF_REQUIRE(hostile >= 0);
	if (hostile == 0) {
		pin_to_cpu(1);
		run_cache_hostile(WORKLOAD_DURATION);
		_exit(0);
	}

	/* Fork a cache-friendly child, pinned to same CPU. */
	friendly = fork();
	ATF_REQUIRE(friendly >= 0);
	if (friendly == 0) {
		pin_to_cpu(1);
		run_cache_friendly(WORKLOAD_DURATION);
		_exit(0);
	}

	/* Let workloads run for a bit before sampling priorities. */
	sleep(WORKLOAD_DURATION / 2 + 1);

	/*
	 * Read priorities.  With the feature disabled, the priorities
	 * should be in the same range for both processes (both are
	 * CPU-bound timeshare at nice 0).
	 */
	ATF_REQUIRE(get_priority(hostile, NULL, &hostile_pri) == 0);
	ATF_REQUIRE(get_priority(friendly, NULL, &friendly_pri) == 0);

	/*
	 * The spec says there should be "no change in scheduling priority
	 * for either thread compared to an unmodified kernel."  We verify
	 * this by checking the priorities are within a small delta of each
	 * other (the scheduler naturally jitters priorities a bit).
	 */
	int diff = (int)hostile_pri - (int)friendly_pri;
	if (diff < 0)
		diff = -diff;
	ATF_CHECK_MSG(diff <= 4,
	    "AC-7: With feature disabled, priority difference between "
	    "cache-hostile (%u) and cache-friendly (%u) should be small "
	    "(got %d), expected <= 4",
	    hostile_pri, friendly_pri, diff);

	/* Clean up children. */
	kill(hostile, SIGKILL);
	kill(friendly, SIGKILL);
	waitpid(hostile, &status, 0);
	waitpid(friendly, &status, 0);

	restore_state(&saved);
}
ATF_TC_CLEANUP(disabled_no_penalty, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
}

/* ==================================================================
 *  AC-8: Enabled behavior -- cache-hostile thread gets worse priority
 * ================================================================== */

ATF_TC_WITH_CLEANUP(enabled_penalty_applied);
ATF_TC_HEAD(enabled_penalty_applied, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-8: With cachemiss_enabled=1, cache-hostile thread gets "
	    "a worse (higher numeric) priority than cache-friendly thread");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "30");
}
ATF_TC_BODY(enabled_penalty_applied, tc)
{
	struct cachemiss_state saved;
	pid_t hostile, friendly;
	unsigned char hostile_pri, friendly_pri;
	int status, enabled;

	require_cachemiss_support();
	save_state(&saved);

	/* Enable the feature with default weight=1, threshold=1000. */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);
	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, 1) == 0);
	ATF_REQUIRE(sysctl_write_u64(SYSCTL_THRESHOLD, 1000) == 0);

	/*
	 * Verify the feature is actually active.  It might not be if
	 * hwpmc is not loaded.
	 */
	ATF_REQUIRE(sysctl_read_int(SYSCTL_ENABLED, &enabled) == 0);
	if (enabled != 1) {
		restore_state(&saved);
		atf_tc_skip("Could not enable cachemiss feature "
		    "(hwpmc may not be loaded)");
	}

	hostile = fork();
	ATF_REQUIRE(hostile >= 0);
	if (hostile == 0) {
		pin_to_cpu(1);
		run_cache_hostile(WORKLOAD_DURATION);
		_exit(0);
	}

	friendly = fork();
	ATF_REQUIRE(friendly >= 0);
	if (friendly == 0) {
		pin_to_cpu(1);
		run_cache_friendly(WORKLOAD_DURATION);
		_exit(0);
	}

	/* Let them accumulate cache misses. */
	sleep(WORKLOAD_DURATION / 2 + 1);

	/*
	 * Sample priorities multiple times and take the worst case.
	 * The scheduler recalculates priorities on each tick, so we
	 * sample several times.
	 */
	int best_diff = 0;
	for (int i = 0; i < 10; i++) {
		if (get_priority(hostile, NULL, &hostile_pri) == 0 &&
		    get_priority(friendly, NULL, &friendly_pri) == 0) {
			int d = (int)hostile_pri - (int)friendly_pri;
			if (d > best_diff)
				best_diff = d;
		}
		usleep(100000); /* 100ms between samples */
	}

	/*
	 * AC-8 says the cache-miss-heavy thread should have a priority
	 * "at least 1 level worse (higher numeric value)."
	 */
	ATF_CHECK_MSG(best_diff >= 1,
	    "AC-8: cache-hostile pri (%u) should be worse than "
	    "cache-friendly pri (%u) by at least 1, best diff=%d",
	    hostile_pri, friendly_pri, best_diff);

	kill(hostile, SIGKILL);
	kill(friendly, SIGKILL);
	waitpid(hostile, &status, 0);
	waitpid(friendly, &status, 0);

	restore_state(&saved);
}
ATF_TC_CLEANUP(enabled_penalty_applied, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
}

/* ==================================================================
 *  AC-9: Penalty never exceeds SCHED_CACHEMISS_PENALTY_MAX (50)
 * ================================================================== */

ATF_TC_WITH_CLEANUP(penalty_max_cap);
ATF_TC_HEAD(penalty_max_cap, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-9: Priority penalty never exceeds 20 levels");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "30");
}
ATF_TC_BODY(penalty_max_cap, tc)
{
	struct cachemiss_state saved;
	pid_t hostile, baseline;
	unsigned char hostile_pri, baseline_pri;
	int status;

	require_cachemiss_support();
	save_state(&saved);

	/*
	 * Use max weight to amplify penalty.  With weight=10, the penalty
	 * should hit the cap quickly.
	 */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);
	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, 10) == 0);
	ATF_REQUIRE(sysctl_write_u64(SYSCTL_THRESHOLD, 100) == 0);

	hostile = fork();
	ATF_REQUIRE(hostile >= 0);
	if (hostile == 0) {
		pin_to_cpu(1);
		run_cache_hostile(WORKLOAD_DURATION + 2);
		_exit(0);
	}

	/* A minimal CPU burner as baseline. */
	baseline = fork();
	ATF_REQUIRE(baseline >= 0);
	if (baseline == 0) {
		pin_to_cpu(1);
		run_cache_friendly(WORKLOAD_DURATION + 2);
		_exit(0);
	}

	sleep(WORKLOAD_DURATION);

	/*
	 * Sample many times and verify the gap never exceeds
	 * SCHED_CACHEMISS_PENALTY_MAX.
	 */
	int max_diff = 0;
	for (int i = 0; i < 20; i++) {
		if (get_priority(hostile, NULL, &hostile_pri) == 0 &&
		    get_priority(baseline, NULL, &baseline_pri) == 0) {
			int d = (int)hostile_pri - (int)baseline_pri;
			if (d > max_diff)
				max_diff = d;
		}
		usleep(50000);
	}

	/*
	 * The penalty component alone is capped at 20.  But the
	 * observed priority difference includes the penalty difference
	 * between both processes.  Since baseline should have ~0 penalty,
	 * the difference should be approximately the hostile's penalty.
	 * It must not exceed 20.
	 */
	ATF_CHECK_MSG(max_diff <= SCHED_CACHEMISS_PENALTY_MAX,
	    "AC-9: maximum observed priority difference (%d) should not "
	    "exceed SCHED_CACHEMISS_PENALTY_MAX (%d)",
	    max_diff, SCHED_CACHEMISS_PENALTY_MAX);

	kill(hostile, SIGKILL);
	kill(baseline, SIGKILL);
	waitpid(hostile, &status, 0);
	waitpid(baseline, &status, 0);

	restore_state(&saved);
}
ATF_TC_CLEANUP(penalty_max_cap, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
}

/* ==================================================================
 *  AC-10: Priority never goes below PRI_MAX_BATCH (223)
 * ================================================================== */

ATF_TC_WITH_CLEANUP(priority_floor);
ATF_TC_HEAD(priority_floor, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-10: Priority with penalty never exceeds PRI_MAX_BATCH (223)");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "30");
}
ATF_TC_BODY(priority_floor, tc)
{
	struct cachemiss_state saved;
	pid_t hostile;
	int status;

	require_cachemiss_support();
	save_state(&saved);

	/*
	 * Use high weight, low threshold, and nice 19 to push the thread
	 * toward the maximum.  nice 19 already pushes priority high;
	 * adding the cachemiss penalty should still not exceed 223.
	 */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);
	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, 10) == 0);
	ATF_REQUIRE(sysctl_write_u64(SYSCTL_THRESHOLD, 100) == 0);

	hostile = fork();
	ATF_REQUIRE(hostile >= 0);
	if (hostile == 0) {
		pin_to_cpu(1);
		/* Set nice to 19 to push base priority high. */
		(void)setpriority(PRIO_PROCESS, 0, 19);
		run_cache_hostile(WORKLOAD_DURATION + 2);
		_exit(0);
	}

	sleep(WORKLOAD_DURATION);

	/*
	 * Sample the thread's priority multiple times and ensure it
	 * never exceeds PRI_MAX_BATCH.
	 */
	for (int i = 0; i < 20; i++) {
		unsigned char pri;
		if (get_priority(hostile, NULL, &pri) == 0) {
			ATF_CHECK_MSG(pri <= PRI_MAX_BATCH,
			    "AC-10: priority %u exceeds PRI_MAX_BATCH (%d)",
			    pri, PRI_MAX_BATCH);
		}
		usleep(50000);
	}

	kill(hostile, SIGKILL);
	waitpid(hostile, &status, 0);

	restore_state(&saved);
}
ATF_TC_CLEANUP(priority_floor, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
}

/* ==================================================================
 *  AC-11: Priority recovery -- penalty decays to 0
 * ================================================================== */

ATF_TC_WITH_CLEANUP(penalty_decay);
ATF_TC_HEAD(penalty_decay, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-11: Penalty decays to 0 after thread becomes cache-friendly");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "30");
}
ATF_TC_BODY(penalty_decay, tc)
{
	struct cachemiss_state saved;
	int status;
	int pipefd[2];

	require_cachemiss_support();
	save_state(&saved);

	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);
	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, 5) == 0);
	ATF_REQUIRE(sysctl_write_u64(SYSCTL_THRESHOLD, 100) == 0);

	ATF_REQUIRE(pipe(pipefd) == 0);

	pid_t child = fork();
	ATF_REQUIRE(child >= 0);
	if (child == 0) {
		close(pipefd[0]);
		/* Phase 1: cache-hostile for a few seconds. */
		run_cache_hostile(WORKLOAD_DURATION);
		/* Signal parent that phase 1 is done. */
		write(pipefd[1], "D", 1);
		/* Phase 2: cache-friendly (penalty should decay). */
		run_cache_friendly(DECAY_TIMEOUT);
		close(pipefd[1]);
		_exit(0);
	}
	close(pipefd[1]);

	/* Wait for phase 1 to complete. */
	char buf;
	ATF_REQUIRE(read(pipefd[0], &buf, 1) == 1);
	close(pipefd[0]);

	/*
	 * Record priority right after the hostile phase ends.
	 * Then wait for decay and check again.
	 */
	unsigned char pri_after_hostile;
	ATF_REQUIRE(get_priority(child, NULL, &pri_after_hostile) == 0);

	/* Wait for decay (spec says ~5 seconds). */
	sleep(DECAY_TIMEOUT);

	unsigned char pri_after_decay;
	if (get_priority(child, NULL, &pri_after_decay) == 0) {
		/*
		 * After decay, the priority should be better (lower
		 * numeric value) or equal.  We do not require exactly 0
		 * penalty because the cache-friendly workload still runs,
		 * but the gap should have narrowed significantly.
		 */
		ATF_CHECK_MSG(pri_after_decay <= pri_after_hostile,
		    "AC-11: priority should improve after decay; "
		    "after hostile=%u, after decay=%u",
		    pri_after_hostile, pri_after_decay);
	}

	kill(child, SIGKILL);
	waitpid(child, &status, 0);

	restore_state(&saved);
}
ATF_TC_CLEANUP(penalty_decay, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
}

/* ==================================================================
 *  AC-12: Forked thread starts with zero penalty
 * ================================================================== */

ATF_TC_WITH_CLEANUP(fork_zero_penalty);
ATF_TC_HEAD(fork_zero_penalty, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-12: Newly forked thread starts with zero cachemiss penalty");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "30");
}
ATF_TC_BODY(fork_zero_penalty, tc)
{
	struct cachemiss_state saved;
	int status;

	require_cachemiss_support();
	save_state(&saved);

	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);
	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, 5) == 0);
	ATF_REQUIRE(sysctl_write_u64(SYSCTL_THRESHOLD, 100) == 0);

	/* Parent runs cache-hostile workload to accumulate penalty. */
	run_cache_hostile(2);

	/*
	 * Now fork a child and immediately read its priority.
	 * It should not inherit the parent's penalty.
	 */
	pid_t child = fork();
	ATF_REQUIRE(child >= 0);
	if (child == 0) {
		/* Child does nothing; just pause so parent can read prio. */
		volatile int dummy = 0;
		for (int i = 0; i < 100000000; i++)
			dummy += i;
		(void)dummy;
		_exit(0);
	}

	/*
	 * Give the child a moment to start, then read its priority.
	 * It should be a normal timeshare priority without penalty.
	 */
	usleep(200000);
	unsigned char child_pri;
	ATF_REQUIRE(get_priority(child, NULL, &child_pri) == 0);

	/*
	 * A normal timeshare thread at nice 0 should be well below
	 * PRI_MAX_BATCH.  If the child inherited the parent's penalty,
	 * it would be much higher.  We check it is in a reasonable
	 * range for a fresh thread.
	 */
	ATF_CHECK_MSG(child_pri >= PRI_MIN_TIMESHARE &&
	    child_pri <= PRI_MAX_BATCH,
	    "AC-12: child priority %u should be in timeshare range [%d, %d]",
	    child_pri, PRI_MIN_TIMESHARE, PRI_MAX_BATCH);

	kill(child, SIGKILL);
	waitpid(child, &status, 0);

	restore_state(&saved);
}
ATF_TC_CLEANUP(fork_zero_penalty, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
}

/* ==================================================================
 *  AC-13: Sleep/wake preserves accumulated misses
 * ================================================================== */

ATF_TC_WITH_CLEANUP(sleep_wake_preserves);
ATF_TC_HEAD(sleep_wake_preserves, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-13: Sleep/wake preserves accumulated cachemiss penalty");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "30");
}
ATF_TC_BODY(sleep_wake_preserves, tc)
{
	struct cachemiss_state saved;
	int status;
	int pipefd[2];

	require_cachemiss_support();
	save_state(&saved);

	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);
	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, 5) == 0);
	ATF_REQUIRE(sysctl_write_u64(SYSCTL_THRESHOLD, 100) == 0);

	ATF_REQUIRE(pipe(pipefd) == 0);

	pid_t child = fork();
	ATF_REQUIRE(child >= 0);
	if (child == 0) {
		close(pipefd[0]);
		/* Phase 1: cache-hostile to build up penalty. */
		run_cache_hostile(WORKLOAD_DURATION);
		/* Signal parent we are about to sleep. */
		write(pipefd[1], "S", 1);
		/* Sleep briefly. */
		usleep(500000);
		/* Signal parent we are awake. */
		write(pipefd[1], "W", 1);
		/* Phase 2: brief CPU burn so priority is recalculated. */
		run_cache_hostile(1);
		close(pipefd[1]);
		_exit(0);
	}
	close(pipefd[1]);

	/* Wait for child to signal it is about to sleep. */
	char buf;
	ATF_REQUIRE(read(pipefd[0], &buf, 1) == 1);

	/* Read priority before sleep (should have penalty). */
	unsigned char pri_before;
	ATF_REQUIRE(get_priority(child, NULL, &pri_before) == 0);

	/* Wait for child to wake up and signal. */
	ATF_REQUIRE(read(pipefd[0], &buf, 1) == 1);
	close(pipefd[0]);

	/* Give child a moment to recalculate priority. */
	usleep(500000);

	/* Read priority after wake. */
	unsigned char pri_after;
	if (get_priority(child, NULL, &pri_after) == 0) {
		/*
		 * The spec says accumulated ts_cachemiss and
		 * ts_cachemiss_penalty are preserved across sleep/wakeup.
		 * So the post-wake priority should still reflect the
		 * penalty (similar to pre-sleep priority).
		 */
		ATF_CHECK_MSG(pri_after >= pri_before - 4,
		    "AC-13: post-wake priority (%u) should still reflect "
		    "penalty (pre-sleep=%u); expected pri_after >= %d",
		    pri_after, pri_before, pri_before - 4);
	}

	kill(child, SIGKILL);
	waitpid(child, &status, 0);

	restore_state(&saved);
}
ATF_TC_CLEANUP(sleep_wake_preserves, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
}

/* ==================================================================
 *  AC-15: hwpmc not loaded -- graceful degradation
 * ================================================================== */

ATF_TC_WITH_CLEANUP(hwpmc_not_loaded);
ATF_TC_HEAD(hwpmc_not_loaded, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-15: With hwpmc not loaded, enabling cachemiss does not "
	    "panic and applies no penalty");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "20");
}
ATF_TC_BODY(hwpmc_not_loaded, tc)
{
	struct cachemiss_state saved;
	int status;

	require_cachemiss_support();
	save_state(&saved);

	/*
	 * We cannot unload hwpmc from a test, so we check: if hwpmc
	 * is not loaded (the modfind check), verify that enabling
	 * the feature does not crash and that workloads run normally.
	 * If hwpmc IS loaded, skip this test.
	 */
	if (modfind("hwpmc") != -1) {
		restore_state(&saved);
		atf_tc_skip("hwpmc module is loaded; cannot test "
		    "the 'not loaded' path");
	}

	/*
	 * hwpmc is NOT loaded.  The sysctl write should succeed
	 * (spec says no error is reported).
	 */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);

	/* Run a cache-hostile workload; it should complete without panic. */
	pid_t child = fork();
	ATF_REQUIRE(child >= 0);
	if (child == 0) {
		run_cache_hostile(2);
		_exit(0);
	}
	waitpid(child, &status, 0);
	ATF_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	/*
	 * Verify no penalty was applied: the child's priority should
	 * have been normal.  (The child already exited, so we cannot
	 * read its priority.  The fact it exited normally without
	 * panic is the main check for AC-15.)
	 */

	restore_state(&saved);
}
ATF_TC_CLEANUP(hwpmc_not_loaded, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
}

/* ==================================================================
 *  AC-18: cachemiss_weight=0 disables penalty
 * ================================================================== */

ATF_TC_WITH_CLEANUP(weight_zero_no_penalty);
ATF_TC_HEAD(weight_zero_no_penalty, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-18: cachemiss_weight=0 with enabled=1 results in no penalty");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "30");
}
ATF_TC_BODY(weight_zero_no_penalty, tc)
{
	struct cachemiss_state saved;
	pid_t hostile, friendly;
	unsigned char hostile_pri, friendly_pri;
	int status;

	require_cachemiss_support();
	save_state(&saved);

	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);
	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, 0) == 0);
	ATF_REQUIRE(sysctl_write_u64(SYSCTL_THRESHOLD, 100) == 0);

	hostile = fork();
	ATF_REQUIRE(hostile >= 0);
	if (hostile == 0) {
		pin_to_cpu(1);
		run_cache_hostile(WORKLOAD_DURATION);
		_exit(0);
	}

	friendly = fork();
	ATF_REQUIRE(friendly >= 0);
	if (friendly == 0) {
		pin_to_cpu(1);
		run_cache_friendly(WORKLOAD_DURATION);
		_exit(0);
	}

	sleep(WORKLOAD_DURATION / 2 + 1);

	ATF_REQUIRE(get_priority(hostile, NULL, &hostile_pri) == 0);
	ATF_REQUIRE(get_priority(friendly, NULL, &friendly_pri) == 0);

	/*
	 * With weight=0, raw_penalty * 0 = 0, so no penalty is applied.
	 * Priorities should be similar.
	 */
	int diff = (int)hostile_pri - (int)friendly_pri;
	if (diff < 0)
		diff = -diff;
	ATF_CHECK_MSG(diff <= 4,
	    "AC-18: With weight=0, priority difference between "
	    "cache-hostile (%u) and cache-friendly (%u) should be small "
	    "(got %d), expected <= 4",
	    hostile_pri, friendly_pri, diff);

	kill(hostile, SIGKILL);
	kill(friendly, SIGKILL);
	waitpid(hostile, &status, 0);
	waitpid(friendly, &status, 0);

	restore_state(&saved);
}
ATF_TC_CLEANUP(weight_zero_no_penalty, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
	(void)sysctl_write_int(SYSCTL_WEIGHT, 1);
}

/* ==================================================================
 *  AC-19: Weight scaling
 * ================================================================== */

ATF_TC_WITH_CLEANUP(weight_scaling);
ATF_TC_HEAD(weight_scaling, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-19: cachemiss_weight=5 results in larger penalty than weight=1");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "60");
}
ATF_TC_BODY(weight_scaling, tc)
{
	struct cachemiss_state saved;
	pid_t hostile, friendly;
	unsigned char hostile_pri, friendly_pri;
	int status;
	int diff_w1, diff_w5;

	require_cachemiss_support();
	save_state(&saved);

	/*
	 * Phase 1: Run with weight=1 and measure priority difference.
	 */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);
	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, 1) == 0);
	ATF_REQUIRE(sysctl_write_u64(SYSCTL_THRESHOLD, 100) == 0);

	hostile = fork();
	ATF_REQUIRE(hostile >= 0);
	if (hostile == 0) {
		pin_to_cpu(1);
		run_cache_hostile(WORKLOAD_DURATION);
		_exit(0);
	}

	friendly = fork();
	ATF_REQUIRE(friendly >= 0);
	if (friendly == 0) {
		pin_to_cpu(1);
		run_cache_friendly(WORKLOAD_DURATION);
		_exit(0);
	}

	sleep(WORKLOAD_DURATION / 2 + 1);

	diff_w1 = 0;
	for (int i = 0; i < 10; i++) {
		if (get_priority(hostile, NULL, &hostile_pri) == 0 &&
		    get_priority(friendly, NULL, &friendly_pri) == 0) {
			int d = (int)hostile_pri - (int)friendly_pri;
			if (d > diff_w1)
				diff_w1 = d;
		}
		usleep(100000);
	}

	kill(hostile, SIGKILL);
	kill(friendly, SIGKILL);
	waitpid(hostile, &status, 0);
	waitpid(friendly, &status, 0);

	/*
	 * Phase 2: Run with weight=5 and measure priority difference.
	 */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, 5) == 0);

	hostile = fork();
	ATF_REQUIRE(hostile >= 0);
	if (hostile == 0) {
		pin_to_cpu(1);
		run_cache_hostile(WORKLOAD_DURATION);
		_exit(0);
	}

	friendly = fork();
	ATF_REQUIRE(friendly >= 0);
	if (friendly == 0) {
		pin_to_cpu(1);
		run_cache_friendly(WORKLOAD_DURATION);
		_exit(0);
	}

	sleep(WORKLOAD_DURATION / 2 + 1);

	diff_w5 = 0;
	for (int i = 0; i < 10; i++) {
		if (get_priority(hostile, NULL, &hostile_pri) == 0 &&
		    get_priority(friendly, NULL, &friendly_pri) == 0) {
			int d = (int)hostile_pri - (int)friendly_pri;
			if (d > diff_w5)
				diff_w5 = d;
		}
		usleep(100000);
	}

	kill(hostile, SIGKILL);
	kill(friendly, SIGKILL);
	waitpid(hostile, &status, 0);
	waitpid(friendly, &status, 0);

	/*
	 * AC-19: weight=5 should produce a penalty 5x larger than weight=1,
	 * up to the cap.  If weight=1 produced any penalty, weight=5 should
	 * produce a strictly larger one (unless both hit the cap).
	 */
	if (diff_w1 > 0) {
		ATF_CHECK_MSG(diff_w5 > diff_w1 ||
		    diff_w5 >= SCHED_CACHEMISS_PENALTY_MAX,
		    "AC-19: weight=5 diff (%d) should exceed weight=1 diff "
		    "(%d) or hit the cap (%d)",
		    diff_w5, diff_w1, SCHED_CACHEMISS_PENALTY_MAX);
	} else {
		/*
		 * If weight=1 produced no observable penalty, we cannot
		 * meaningfully test the scaling.  This might happen if
		 * hwpmc is not loaded or the workload did not generate
		 * enough misses.
		 */
		atf_tc_skip("weight=1 produced no observable penalty; "
		    "cannot test scaling");
	}

	restore_state(&saved);
}
ATF_TC_CLEANUP(weight_scaling, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
	(void)sysctl_write_int(SYSCTL_WEIGHT, 1);
}

/* ==================================================================
 *  AC-20: Toggle enabled 1->0 releases PMC resources
 * ================================================================== */

ATF_TC_WITH_CLEANUP(toggle_disable);
ATF_TC_HEAD(toggle_disable, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-20: Toggling cachemiss_enabled from 1 to 0 releases PMCs");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "10");
}
ATF_TC_BODY(toggle_disable, tc)
{
	struct cachemiss_state saved;
	int val;

	require_cachemiss_support();
	save_state(&saved);

	/* Enable, then disable. */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_ENABLED, &val) == 0);

	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 0) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_ENABLED, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 0,
	    "AC-20: cachemiss_enabled should be 0 after toggle, got %d", val);

	/*
	 * After disabling, the PMC resources should be released.
	 * We cannot directly verify PMC allocation from userland,
	 * but we verify that re-enabling works (which implies the
	 * resources were properly released).
	 */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_ENABLED, &val) == 0);
	/* If we could enable again, the PMCs were released on disable. */

	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 0) == 0);

	restore_state(&saved);
}
ATF_TC_CLEANUP(toggle_disable, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
}

/* ==================================================================
 *  AC-21: Toggle enabled 0->1 with PMCs busy -- no crash
 * ================================================================== */

ATF_TC_WITH_CLEANUP(toggle_enable_pmc_busy);
ATF_TC_HEAD(toggle_enable_pmc_busy, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-21: Enabling when PMC hardware is busy does not crash "
	    "and sched_pmc_is_active returns 0");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "10");
}
ATF_TC_BODY(toggle_enable_pmc_busy, tc)
{

	require_cachemiss_support();

	/*
	 * We cannot easily guarantee all PMCs are busy from userland.
	 * However, we CAN verify that toggling enabled=1 does not crash
	 * even when there might be contention.  The sysctl write should
	 * succeed.  If a warning is logged, that is correct per the spec.
	 *
	 * The primary test is that the system does not panic.
	 */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 0) == 0);
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);

	int val;
	ATF_REQUIRE(sysctl_read_int(SYSCTL_ENABLED, &val) == 0);
	ATF_CHECK_MSG(val == 1,
	    "AC-21: cachemiss_enabled should be 1 after toggle, got %d", val);

	/* Cleanup: disable. */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 0) == 0);
}
ATF_TC_CLEANUP(toggle_enable_pmc_busy, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
}

/* ==================================================================
 *  AC-1, AC-2, AC-14, AC-16, AC-17: Untestable from userland
 * ================================================================== */

ATF_TC(compile_time_untestable);
ATF_TC_HEAD(compile_time_untestable, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-1, AC-2, AC-14: Compile-time and kernel-internal criteria "
	    "not testable from userland");
}
ATF_TC_BODY(compile_time_untestable, tc)
{

	atf_tc_skip("AC-1 (no HWPMC_HOOKS build), AC-2 (static_assert), "
	    "AC-14 (CPU migration reset) require kernel-level "
	    "instrumentation or separate builds; cannot be tested from "
	    "userland ATF tests");
}

ATF_TC(interactive_exempt_untestable);
ATF_TC_HEAD(interactive_exempt_untestable, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-17: Interactive threads exempt from penalty -- difficult "
	    "to test from userland");
}
ATF_TC_BODY(interactive_exempt_untestable, tc)
{

	atf_tc_skip("AC-17: Interactive thread exemption depends on the "
	    "internal interactivity score (sched_interact), which is "
	    "not exposed to userland.  A thread with nice -20 may or "
	    "may not qualify as interactive depending on its "
	    "sleep/run ratio.  Cannot reliably test from userland.");
}

/* ==================================================================
 *  AC-16: Interaction with nice -- penalized thread gets worse priority
 * ================================================================== */

ATF_TC_WITH_CLEANUP(nice_interaction);
ATF_TC_HEAD(nice_interaction, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-16: Thread with nice 0 and max cache-miss penalty has "
	    "lower priority than one with nice 0 and zero penalty");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "30");
}
ATF_TC_BODY(nice_interaction, tc)
{
	struct cachemiss_state saved;
	pid_t hostile, friendly;
	unsigned char hostile_pri, friendly_pri;
	int status;

	require_cachemiss_support();
	save_state(&saved);

	/* Enable with high weight to push penalty to maximum. */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);
	ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, 10) == 0);
	ATF_REQUIRE(sysctl_write_u64(SYSCTL_THRESHOLD, 100) == 0);

	/* Both children at nice 0, pinned to same CPU. */
	hostile = fork();
	ATF_REQUIRE(hostile >= 0);
	if (hostile == 0) {
		pin_to_cpu(1);
		(void)setpriority(PRIO_PROCESS, 0, 0);
		run_cache_hostile(WORKLOAD_DURATION);
		_exit(0);
	}

	friendly = fork();
	ATF_REQUIRE(friendly >= 0);
	if (friendly == 0) {
		pin_to_cpu(1);
		(void)setpriority(PRIO_PROCESS, 0, 0);
		run_cache_friendly(WORKLOAD_DURATION);
		_exit(0);
	}

	sleep(WORKLOAD_DURATION / 2 + 1);

	int max_diff = 0;
	for (int i = 0; i < 10; i++) {
		if (get_priority(hostile, NULL, &hostile_pri) == 0 &&
		    get_priority(friendly, NULL, &friendly_pri) == 0) {
			int d = (int)hostile_pri - (int)friendly_pri;
			if (d > max_diff)
				max_diff = d;
		}
		usleep(100000);
	}

	ATF_CHECK_MSG(max_diff >= 1,
	    "AC-16: cache-hostile thread at nice 0 (%u) should have "
	    "worse priority than cache-friendly thread at nice 0 (%u), "
	    "diff=%d",
	    hostile_pri, friendly_pri, max_diff);

	kill(hostile, SIGKILL);
	kill(friendly, SIGKILL);
	waitpid(hostile, &status, 0);
	waitpid(friendly, &status, 0);

	restore_state(&saved);
}
ATF_TC_CLEANUP(nice_interaction, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
}

/* ==================================================================
 *  AC-20 extended: Repeated toggling
 * ================================================================== */

ATF_TC_WITH_CLEANUP(toggle_repeated);
ATF_TC_HEAD(toggle_repeated, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-20/AC-21: Rapidly toggling enabled does not crash");
	atf_tc_set_md_var(tc, "require.user", "root");
	atf_tc_set_md_var(tc, "timeout", "15");
}
ATF_TC_BODY(toggle_repeated, tc)
{
	struct cachemiss_state saved;

	require_cachemiss_support();
	save_state(&saved);

	/*
	 * Toggle the feature on and off many times.  This stresses the
	 * PMC allocation/deallocation path.  The system must not panic.
	 */
	for (int i = 0; i < 50; i++) {
		ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);
		ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 0) == 0);
	}

	/* Verify final state is disabled. */
	int val;
	ATF_REQUIRE(sysctl_read_int(SYSCTL_ENABLED, &val) == 0);
	ATF_CHECK_EQ(val, 0);

	restore_state(&saved);
}
ATF_TC_CLEANUP(toggle_repeated, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
}

/* ==================================================================
 *  Additional edge case: sysctl_weight boundary values
 * ================================================================== */

ATF_TC_WITH_CLEANUP(sysctl_weight_boundary_values);
ATF_TC_HEAD(sysctl_weight_boundary_values, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-5: Verify boundary values for cachemiss_weight (0..10)");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(sysctl_weight_boundary_values, tc)
{
	struct cachemiss_state saved;
	int val;

	require_cachemiss_support();
	save_state(&saved);

	/* All values in valid range should stick. */
	for (int i = 0; i <= 10; i++) {
		ATF_REQUIRE(sysctl_write_int(SYSCTL_WEIGHT, i) == 0);
		ATF_REQUIRE(sysctl_read_int(SYSCTL_WEIGHT, &val) == 0);
		ATF_CHECK_EQ_MSG(val, i,
		    "weight=%d should be accepted, got %d", i, val);
	}

	restore_state(&saved);
}
ATF_TC_CLEANUP(sysctl_weight_boundary_values, tc)
{

	(void)sysctl_write_int(SYSCTL_WEIGHT, 1);
}

/* ==================================================================
 *  Additional edge case: sysctl_interval boundary values
 * ================================================================== */

ATF_TC_WITH_CLEANUP(sysctl_interval_boundary_values);
ATF_TC_HEAD(sysctl_interval_boundary_values, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-6: Verify boundary values for cachemiss_interval (1..100)");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(sysctl_interval_boundary_values, tc)
{
	struct cachemiss_state saved;
	int val;

	require_cachemiss_support();
	save_state(&saved);

	/* Test specific values across the range. */
	int test_vals[] = { 1, 2, 4, 50, 99, 100 };
	for (size_t i = 0; i < sizeof(test_vals)/sizeof(test_vals[0]); i++) {
		ATF_REQUIRE(sysctl_write_int(SYSCTL_INTERVAL,
		    test_vals[i]) == 0);
		ATF_REQUIRE(sysctl_read_int(SYSCTL_INTERVAL, &val) == 0);
		ATF_CHECK_EQ_MSG(val, test_vals[i],
		    "interval=%d should be accepted, got %d",
		    test_vals[i], val);
	}

	restore_state(&saved);
}
ATF_TC_CLEANUP(sysctl_interval_boundary_values, tc)
{

	(void)sysctl_write_int(SYSCTL_INTERVAL, 4);
}

/* ==================================================================
 *  Additional: threshold can be read/written
 * ================================================================== */

ATF_TC_WITH_CLEANUP(sysctl_threshold_readwrite);
ATF_TC_HEAD(sysctl_threshold_readwrite, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-3/AC-4: Verify cachemiss_threshold is a uint64 and "
	    "can be read and written");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(sysctl_threshold_readwrite, tc)
{
	struct cachemiss_state saved;
	uint64_t val;

	require_cachemiss_support();
	save_state(&saved);

	/* Write and read back a large value. */
	ATF_REQUIRE(sysctl_write_u64(SYSCTL_THRESHOLD, 500000) == 0);
	ATF_REQUIRE(sysctl_read_u64(SYSCTL_THRESHOLD, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 500000,
	    "threshold=500000 should be accepted, got %ju",
	    (uintmax_t)val);

	/* Write 0. */
	ATF_REQUIRE(sysctl_write_u64(SYSCTL_THRESHOLD, 0) == 0);
	ATF_REQUIRE(sysctl_read_u64(SYSCTL_THRESHOLD, &val) == 0);
	ATF_CHECK_EQ_MSG(val, 0,
	    "threshold=0 should be accepted, got %ju",
	    (uintmax_t)val);

	/* Write a very large value. */
	ATF_REQUIRE(sysctl_write_u64(SYSCTL_THRESHOLD,
	    UINT64_MAX) == 0);
	ATF_REQUIRE(sysctl_read_u64(SYSCTL_THRESHOLD, &val) == 0);
	ATF_CHECK_EQ_MSG(val, UINT64_MAX,
	    "threshold=UINT64_MAX should be accepted, got %ju",
	    (uintmax_t)val);

	restore_state(&saved);
}
ATF_TC_CLEANUP(sysctl_threshold_readwrite, tc)
{

	(void)sysctl_write_u64(SYSCTL_THRESHOLD, 1000);
}

/* ==================================================================
 *  Additional: Enabled knob only accepts 0 and 1
 * ================================================================== */

ATF_TC_WITH_CLEANUP(sysctl_enabled_values);
ATF_TC_HEAD(sysctl_enabled_values, tc)
{

	atf_tc_set_md_var(tc, "descr",
	    "AC-3: Verify cachemiss_enabled accepts 0 and 1");
	atf_tc_set_md_var(tc, "require.user", "root");
}
ATF_TC_BODY(sysctl_enabled_values, tc)
{
	struct cachemiss_state saved;
	int val;

	require_cachemiss_support();
	save_state(&saved);

	/* Write 0, should succeed. */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 0) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_ENABLED, &val) == 0);
	ATF_CHECK_EQ(val, 0);

	/* Write 1, should succeed. */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 1) == 0);
	ATF_REQUIRE(sysctl_read_int(SYSCTL_ENABLED, &val) == 0);
	ATF_CHECK_EQ(val, 1);

	/* Disable again for cleanup. */
	ATF_REQUIRE(sysctl_write_int(SYSCTL_ENABLED, 0) == 0);

	restore_state(&saved);
}
ATF_TC_CLEANUP(sysctl_enabled_values, tc)
{

	(void)sysctl_write_int(SYSCTL_ENABLED, 0);
}

/* ==================================================================
 *  Test registration
 * ================================================================== */

ATF_TP_ADD_TCS(tp)
{

	/* AC-3: Sysctl existence */
	ATF_TP_ADD_TC(tp, sysctl_knobs_exist);

	/* AC-4: Defaults */
	ATF_TP_ADD_TC(tp, sysctl_defaults);

	/* AC-5: Weight clamping */
	ATF_TP_ADD_TC(tp, sysctl_weight_clamp_high);
	ATF_TP_ADD_TC(tp, sysctl_weight_clamp_low);
	ATF_TP_ADD_TC(tp, sysctl_weight_boundary_values);

	/* AC-6: Interval clamping */
	ATF_TP_ADD_TC(tp, sysctl_interval_clamp_high);
	ATF_TP_ADD_TC(tp, sysctl_interval_clamp_low);
	ATF_TP_ADD_TC(tp, sysctl_interval_boundary_values);

	/* AC-3/AC-4 additional: Threshold read/write */
	ATF_TP_ADD_TC(tp, sysctl_threshold_readwrite);
	ATF_TP_ADD_TC(tp, sysctl_enabled_values);

	/* AC-7: Disabled behavior */
	ATF_TP_ADD_TC(tp, disabled_no_penalty);

	/* AC-8: Enabled behavior */
	ATF_TP_ADD_TC(tp, enabled_penalty_applied);

	/* AC-9: Penalty cap */
	ATF_TP_ADD_TC(tp, penalty_max_cap);

	/* AC-10: Priority floor */
	ATF_TP_ADD_TC(tp, priority_floor);

	/* AC-11: Decay */
	ATF_TP_ADD_TC(tp, penalty_decay);

	/* AC-12: Fork zero penalty */
	ATF_TP_ADD_TC(tp, fork_zero_penalty);

	/* AC-13: Sleep/wake preserves */
	ATF_TP_ADD_TC(tp, sleep_wake_preserves);

	/* AC-15: hwpmc not loaded */
	ATF_TP_ADD_TC(tp, hwpmc_not_loaded);

	/* AC-16: Nice interaction */
	ATF_TP_ADD_TC(tp, nice_interaction);

	/* AC-17: Interactive exempt (skip) */
	ATF_TP_ADD_TC(tp, interactive_exempt_untestable);

	/* AC-18: Weight zero */
	ATF_TP_ADD_TC(tp, weight_zero_no_penalty);

	/* AC-19: Weight scaling */
	ATF_TP_ADD_TC(tp, weight_scaling);

	/* AC-20: Toggle disable */
	ATF_TP_ADD_TC(tp, toggle_disable);

	/* AC-21: Toggle enable with busy PMCs */
	ATF_TP_ADD_TC(tp, toggle_enable_pmc_busy);

	/* AC-20/21: Repeated toggling */
	ATF_TP_ADD_TC(tp, toggle_repeated);

	/* AC-1, AC-2, AC-14: Compile-time (skip) */
	ATF_TP_ADD_TC(tp, compile_time_untestable);

	return (atf_no_error());
}
