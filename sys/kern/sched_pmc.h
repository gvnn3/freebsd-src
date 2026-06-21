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
 * Scheduler PMC interface for cache-miss-aware priority adjustment.
 *
 * This header declares the interface used by sched_ule.c to read
 * LLC-miss hardware performance counters.  All functions are compiled
 * only when HWPMC_HOOKS is defined.
 */

#ifndef _KERN_SCHED_PMC_H_
#define	_KERN_SCHED_PMC_H_

#ifdef HWPMC_HOOKS

#include <sys/types.h>

/*
 * sched_pmc_read - Read the current raw LLC-miss counter for the calling CPU.
 *
 * Preconditions:
 *   - Called with the current CPU's tdq lock held (spin-lock context).
 *   - cpu must equal curcpu.
 *   - cachemiss_enabled must be 1.
 *
 * Returns the current raw value of the LLC-miss counter.
 * Returns 0 if hwpmc is not loaded or the PMC could not be allocated.
 *
 * Cost: A single rdpmc instruction on x86 (tens of nanoseconds).
 */
uint64_t	sched_pmc_read(int cpu);

/*
 * sched_pmc_is_active - Check if the cache-miss PMC subsystem is active.
 *
 * Returns 1 if counters are running, 0 otherwise.
 * Safe to call from any context (reads a single int flag).
 */
int		sched_pmc_is_active(void);

/*
 * sched_pmc_init - Allocate per-CPU LLC-miss counters.
 *
 * Programs a system-wide counting-mode PMC on each online CPU for the
 * architecture-appropriate LLC-miss event.  Returns 0 on success,
 * non-zero on failure.
 */
int		sched_pmc_init(void);

/*
 * sched_pmc_fini - Release per-CPU LLC-miss counters.
 *
 * Stops and deallocates the PMCs on all CPUs.
 */
void		sched_pmc_fini(void);

#endif /* HWPMC_HOOKS */

#endif /* _KERN_SCHED_PMC_H_ */
