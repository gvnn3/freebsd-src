/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, George V. Neville-Neil
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Scheduler PMC subsystem for cache-miss-aware priority adjustment.
 *
 * This module manages per-CPU LLC-miss hardware performance counters
 * for use by the ULE scheduler.  It programs a single general-purpose
 * PMC on each CPU to count LLC misses in system-wide counting mode.
 *
 * The counter is read via rdpmc (x86) or equivalent, which is safe
 * to call from spin-lock context (no sleeping, no memory allocation).
 *
 * Compiled only when HWPMC_HOOKS is defined.
 */

#include "opt_hwpmc_hooks.h"

#ifdef HWPMC_HOOKS

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/smp.h>
#include <sys/syslog.h>
#include <sys/pmckern.h>
#include <sys/sched.h>

#include <machine/cpu.h>
#include <machine/cpufunc.h>

#include "sched_pmc.h"

/*
 * Per-CPU state for the scheduler's LLC-miss PMC.
 *
 * We use a single general-purpose programmable PMC (counter index 0)
 * on each CPU.  The counter index used for rdpmc is stored in
 * spc_rdpmc_idx.  On x86, this is the hardware counter number (0).
 *
 * Assumption: We use general-purpose PMC index 0.  This may conflict
 * with other PMC consumers.  The init function checks if the counter
 * is available and fails gracefully if not.
 */
struct sched_pmc_cpu {
	int	spc_active;	/* 1 if this CPU's counter is running */
	int	spc_rdpmc_idx;	/* rdpmc index for reading */
};

static struct sched_pmc_cpu sched_pmc_cpus[MAXCPU];
static volatile int sched_pmc_active;	/* global active flag */

/*
 * Architecture-specific LLC-miss event configuration.
 *
 * Intel: LONGEST_LAT_CACHE.MISS = event 0x2E, umask 0x41
 * AMD:   L3 cache miss data fills = event 0x04, umask 0x01 (Zen/Fam 17h+)
 *        Verification against the specific AMD PPR for the target platform
 *        is recommended, as event semantics can vary across Zen generations.
 * ARM:   LL_CACHE_MISS_RD = event 0x37
 *
 * On x86, we program the PERFEVTSEL MSR directly.  The PERFEVTSEL
 * encoding is:
 *   bits  7:0  = event select
 *   bits 15:8  = unit mask
 *   bit  16    = USR (count in user mode)
 *   bit  17    = OS  (count in kernel mode)
 *   bit  22    = EN  (enable counter)
 *
 * We count both user and kernel LLC misses (USR | OS | EN).
 */

#if defined(__amd64__) || defined(__i386__)

/*
 * Intel PERFEVTSEL MSR base address.  Counter 0's EVSEL is at 0x186,
 * and the corresponding performance counter is at PMC 0x0C1.
 */
#define	SCHED_PMC_INTEL_EVSEL0		0x186
#define	SCHED_PMC_INTEL_PMC0		0x0C1

/*
 * Intel LONGEST_LAT_CACHE.MISS: event=0x2E, umask=0x41.
 * With USR (bit 16), OS (bit 17), EN (bit 22) set.
 */
#define	SCHED_PMC_INTEL_LLC_MISS_EVSEL	\
	(0x2E | (0x41 << 8) | (1 << 16) | (1 << 17) | (1 << 22))

/*
 * AMD core counter MSR base address (Fam 17h+).
 * EVSEL at 0xC0010200 + 2*n, PERFCTR at 0xC0010200 + 2*n + 1.
 * For legacy (K8): EVSEL at 0xC0010000, PERFCTR at 0xC0010004.
 *
 * AMD LLC miss event: We use event 0x04, umask 0x01, which targets
 * L3 cache miss data fills on AMD Zen (Fam 17h+) core performance
 * counters.  Verification against the specific AMD PPR (Processor
 * Programming Reference) for the target platform is recommended,
 * as event semantics can vary across Zen generations.
 */
#define	SCHED_PMC_AMD_CORE_EVSEL0	0xC0010200
#define	SCHED_PMC_AMD_CORE_PMC0		0xC0010201
#define	SCHED_PMC_AMD_LEGACY_EVSEL0	0xC0010000
#define	SCHED_PMC_AMD_LEGACY_PMC0	0xC0010004

/*
 * AMD event encoding for LLC miss.
 * We program the EVSEL register with:
 *   Event 0x04, Umask 0x01 (L3 miss, data cache fills)
 *   USR (bit 16) | OS (bit 17) | EN (bit 22)
 */
#define	SCHED_PMC_AMD_LLC_MISS_EVSEL	\
	(0x04 | (0x01 << 8) | (1 << 16) | (1 << 17) | (1 << 22))

static int sched_pmc_is_intel;
static int sched_pmc_is_amd;
static uint32_t sched_pmc_evsel_msr;
static uint32_t sched_pmc_ctr_msr;
static uint64_t sched_pmc_evsel_val;

static void
sched_pmc_detect_cpu(void)
{
	u_int regs[4];
	u_int base_family, family;
	char vendor[13];

	do_cpuid(0, regs);
	memcpy(vendor, &regs[1], 4);
	memcpy(vendor + 4, &regs[3], 4);
	memcpy(vendor + 8, &regs[2], 4);
	vendor[12] = '\0';

	if (strcmp(vendor, "GenuineIntel") == 0) {
		sched_pmc_is_intel = 1;
		sched_pmc_evsel_msr = SCHED_PMC_INTEL_EVSEL0;
		sched_pmc_ctr_msr = SCHED_PMC_INTEL_PMC0;
		sched_pmc_evsel_val = SCHED_PMC_INTEL_LLC_MISS_EVSEL;
	} else if (strcmp(vendor, "AuthenticAMD") == 0) {
		sched_pmc_is_amd = 1;
		/*
		 * Use core counter base on Fam 17h+ (Zen).
		 * Fall back to legacy MSRs on older AMD.
		 */
		do_cpuid(1, regs);
		/*
		 * Fix 7: Per CPUID spec, ExtendedFamily is only added
		 * when BaseFamily == 0xF.
		 */
		base_family = (regs[0] >> 8) & 0xF;
		family = base_family;
		if (base_family == 0xF)
			family += (regs[0] >> 20) & 0xFF;
		if (family >= 0x17) {
			sched_pmc_evsel_msr = SCHED_PMC_AMD_CORE_EVSEL0;
			sched_pmc_ctr_msr = SCHED_PMC_AMD_CORE_PMC0;
		} else {
			sched_pmc_evsel_msr = SCHED_PMC_AMD_LEGACY_EVSEL0;
			sched_pmc_ctr_msr = SCHED_PMC_AMD_LEGACY_PMC0;
		}
		sched_pmc_evsel_val = SCHED_PMC_AMD_LLC_MISS_EVSEL;
	}
}

/*
 * Program the LLC-miss counter on the current CPU.
 * Must be called with the thread pinned to the target CPU.
 */
static int
sched_pmc_program_cpu(int cpu)
{

	if (!sched_pmc_is_intel && !sched_pmc_is_amd)
		return (ENXIO);

	/* Stop the counter first (clear EN bit). */
	wrmsr(sched_pmc_evsel_msr, 0);

	/* Zero the counter. */
	wrmsr(sched_pmc_ctr_msr, 0);

	/* Program and start the counter. */
	wrmsr(sched_pmc_evsel_msr, sched_pmc_evsel_val);

	sched_pmc_cpus[cpu].spc_rdpmc_idx = 0;
	sched_pmc_cpus[cpu].spc_active = 1;

	return (0);
}

/*
 * Stop the LLC-miss counter on the current CPU.
 * Must be called with the thread pinned to the target CPU.
 */
static void
sched_pmc_stop_cpu(int cpu)
{

	if (sched_pmc_cpus[cpu].spc_active) {
		/* Disable the counter by clearing the EVSEL. */
		wrmsr(sched_pmc_evsel_msr, 0);
		sched_pmc_cpus[cpu].spc_active = 0;
	}
}

/*
 * sched_pmc_read - Read the LLC-miss counter for the specified CPU.
 *
 * Precondition: Called on the local CPU (cpu == curcpu) with
 * the tdq spin lock held.  This is a single rdpmc instruction.
 *
 * Returns 0 if the PMC subsystem is not active or counters are
 * not allocated.
 */
uint64_t
sched_pmc_read(int cpu)
{

	if (__predict_false(!sched_pmc_active))
		return (0);
	if (__predict_false(!sched_pmc_cpus[cpu].spc_active))
		return (0);

	return (rdpmc(sched_pmc_cpus[cpu].spc_rdpmc_idx));
}

#elif defined(__aarch64__)

/*
 * ARM64 PMU event: LL_CACHE_MISS_RD = 0x37
 *
 * We use PMEVCNTR0_EL0 (event counter 0) and program PMEVTYPER0_EL0
 * with the event type.  We also need to enable the counter in
 * PMCNTENSET_EL0.
 */
#define	SCHED_PMC_ARM64_LLC_MISS_EVENT	0x37

static void
sched_pmc_detect_cpu(void)
{
	/* Nothing to detect on ARM64; we use the standard PMU interface. */
}

static int
sched_pmc_program_cpu(int cpu)
{
	uint64_t val;

	/* Select counter 0. */
	WRITE_SPECIALREG(pmselr_el0, 0);
	isb();

	/* Program the event type. */
	WRITE_SPECIALREG(pmxevtyper_el0, SCHED_PMC_ARM64_LLC_MISS_EVENT);
	isb();

	/* Zero the counter. */
	WRITE_SPECIALREG(pmxevcntr_el0, 0);
	isb();

	/* Enable counter 0 in PMCNTENSET_EL0. */
	val = READ_SPECIALREG(pmcntenset_el0);
	WRITE_SPECIALREG(pmcntenset_el0, val | (1UL << 0));
	isb();

	/* Enable the PMU in PMCR_EL0 (set E bit). */
	val = READ_SPECIALREG(pmcr_el0);
	WRITE_SPECIALREG(pmcr_el0, val | 1);
	isb();

	sched_pmc_cpus[cpu].spc_rdpmc_idx = 0;
	sched_pmc_cpus[cpu].spc_active = 1;

	return (0);
}

static void
sched_pmc_stop_cpu(int cpu)
{
	uint64_t val;

	if (sched_pmc_cpus[cpu].spc_active) {
		/* Disable counter 0 in PMCNTENCLR_EL0. */
		val = (1UL << 0);
		WRITE_SPECIALREG(pmcntenclr_el0, val);
		isb();
		sched_pmc_cpus[cpu].spc_active = 0;
	}
}

uint64_t
sched_pmc_read(int cpu)
{

	if (__predict_false(!sched_pmc_active))
		return (0);
	if (__predict_false(!sched_pmc_cpus[cpu].spc_active))
		return (0);

	/* Select counter 0 and read it. */
	WRITE_SPECIALREG(pmselr_el0, 0);
	isb();
	return (READ_SPECIALREG(pmxevcntr_el0));
}

#else /* Unsupported architecture */

static void
sched_pmc_detect_cpu(void)
{
}

static int
sched_pmc_program_cpu(int cpu __unused)
{

	return (ENXIO);
}

static void
sched_pmc_stop_cpu(int cpu __unused)
{
}

uint64_t
sched_pmc_read(int cpu __unused)
{

	return (0);
}

#endif /* architecture selection */

/*
 * sched_pmc_is_active - Check if the PMC subsystem is active.
 *
 * Returns 1 if counters are running, 0 otherwise.
 * Safe to call from any context.
 */
int
sched_pmc_is_active(void)
{

	return (sched_pmc_active);
}

/*
 * sched_pmc_init - Allocate and start per-CPU LLC-miss counters.
 *
 * This function detects the CPU type and programs a general-purpose
 * PMC on each online CPU to count LLC misses.  It must be called
 * from a context that can sleep (smp_rendezvous is used to program
 * each CPU's MSRs).
 *
 * Returns 0 on success, non-zero on failure.
 *
 * AC-15: When hwpmc is not loaded or PMC hardware is unavailable,
 * this logs a warning and returns an error.
 * AC-21: If counters cannot be allocated, returns error, no crash.
 */

static void
sched_pmc_program_ipi(void *arg __unused)
{
	int cpu;

	cpu = PCPU_GET(cpuid);
	sched_pmc_program_cpu(cpu);
}

static void
sched_pmc_stop_ipi(void *arg __unused)
{
	int cpu;

	cpu = PCPU_GET(cpuid);
	sched_pmc_stop_cpu(cpu);
}

int
sched_pmc_init(void)
{
	int error;

	sched_pmc_detect_cpu();

	/*
	 * Fix 5: Pin the current thread so it cannot migrate between
	 * PCPU_GET(cpuid) and the wrmsr() inside sched_pmc_program_cpu().
	 */
	sched_pin();

	/*
	 * Test-program on the current CPU first to verify the hardware
	 * supports the event we need.
	 */
	error = sched_pmc_program_cpu(PCPU_GET(cpuid));
	if (error != 0) {
		sched_unpin();
		/* AC-15, E2: Log warning, return error, no panic. */
		log(LOG_WARNING,
		    "sched_pmc: LLC-miss PMC not available on this platform\n");
		return (error);
	}
	/* Stop the test counter; we'll reprogram all CPUs below. */
	sched_pmc_stop_cpu(PCPU_GET(cpuid));

	sched_unpin();

#ifdef SMP
	/*
	 * Use smp_rendezvous to program counters on all CPUs.
	 * This runs the function on each CPU with interrupts disabled.
	 */
	smp_rendezvous(NULL, sched_pmc_program_ipi, NULL, NULL);
#else
	sched_pmc_program_cpu(0);
#endif

	/* Verify at least one CPU succeeded. */
	if (!sched_pmc_cpus[PCPU_GET(cpuid)].spc_active) {
		log(LOG_WARNING,
		    "sched_pmc: failed to program LLC-miss counter\n");
		return (ENXIO);
	}

	atomic_store_int(&sched_pmc_active, 1);
	return (0);
}

/*
 * sched_pmc_fini - Stop and release per-CPU LLC-miss counters.
 *
 * AC-20: After this call, the PMC hardware counters are free for
 * use by other consumers (e.g., pmcstat(8)).
 */
void
sched_pmc_fini(void)
{

	atomic_store_int(&sched_pmc_active, 0);

#ifdef SMP
	smp_rendezvous(NULL, sched_pmc_stop_ipi, NULL, NULL);
#else
	sched_pmc_stop_cpu(0);
#endif
}

#endif /* HWPMC_HOOKS */
