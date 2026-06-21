# ULE Scheduler Cache-Miss-Aware Priority Adjustment

**Version:** 1.0
**Date:** 2026-06-20
**File:** `spec/sched_ule_cachemiss_priority_v1.md`

---

## Summary

Extend the ULE scheduler (`sys/kern/sched_ule.c`) to use hardware performance
counters (hwpmc(4)) to measure last-level cache (LLC) misses per thread and
fold that measurement into the timeshare priority calculation.  Threads that
incur a high rate of LLC misses receive a priority penalty (higher numeric
priority value = lower scheduling priority), so that cache-friendly threads
are preferentially scheduled.  The feature is compiled in only when the kernel
is built with `options HWPMC_HOOKS`, is disabled at boot by default, and is
controlled entirely through sysctl knobs at runtime.

---

## Interface / API

### New sysctl knobs

All knobs live under the existing `kern.sched.ule` sysctl node.  All are
`int` unless otherwise noted.

| sysctl name | type | default | flags | description |
|---|---|---|---|---|
| `kern.sched.ule.cachemiss_enabled` | int | 0 | `CTLFLAG_RWTUN` | Master enable. 0 = disabled, 1 = enabled. When 0 the scheduler never reads PMCs and never applies any cache-miss penalty. |
| `kern.sched.ule.cachemiss_weight` | int | 1 | `CTLFLAG_RWTUN` | Penalty multiplier. The raw penalty (see Behavior) is multiplied by this value before being added to the priority. Valid range: 0..10. Values outside this range are clamped silently on write. A value of 0 effectively disables the penalty without flipping `cachemiss_enabled`. |
| `kern.sched.ule.cachemiss_threshold` | uint64 | 1000 | `CTLFLAG_RWTUN` | Cache-miss count per sample interval below which no penalty is applied. Threads whose accumulated misses in the current window are at or below this value receive zero penalty. Type is `uint64_t`, exposed via `SYSCTL_U64`. |
| `kern.sched.ule.cachemiss_interval` | int | 4 | `CTLFLAG_RWTUN` | Number of `sched_clock()` ticks between PMC reads. The counter is read every `cachemiss_interval` ticks, not on every tick. Valid range: 1..100. Values outside are clamped. |

### New fields in `struct td_sched`

The following fields are added to `struct td_sched` in
`sys/kern/sched_ule.c`, inside an `#ifdef HWPMC_HOOKS` guard.  All fields
are protected by the thread lock, consistent with the existing fields.

```c
#ifdef HWPMC_HOOKS
    uint64_t    ts_cachemiss;       /* LLC misses accumulated this window. */
    uint64_t    ts_cachemiss_prev;  /* LLC misses at last PMC read. */
    u_int       ts_cachemiss_ticks; /* Ticks since last PMC read. */
    int         ts_cachemiss_penalty; /* Current priority penalty [0..SCHED_CACHEMISS_PENALTY_MAX]. */
#endif
```

**Size impact:** These four fields add 24 bytes (8+8+4+4).  The existing
`struct td_sched` without `KTR` is 34 bytes (with alignment padding likely
40 bytes).  The `thread0_storage.t0st_sched` allocation is 80 bytes.
40 + 24 = 64 bytes, which fits within the 80-byte budget.  The static
assertion on line 111 of `sched_ule.c` will continue to pass.

### New constants

```c
#define SCHED_CACHEMISS_PENALTY_MAX    50
```

This caps the maximum penalty that cache misses can add to a thread's
computed priority.  The cap prevents a cache-miss-heavy thread from being
demoted below `PRI_MAX_BATCH` (the lowest timeshare priority before idle).

---

## Behavior

### Overview of data flow

1. The scheduler's clock handler (`sched_ule_clock()`) is called on every
   `stathz` tick for the currently running thread.
2. Every `cachemiss_interval` ticks, the handler reads the thread's LLC-miss
   PMC counter, computes a delta, and updates `ts_cachemiss`.
3. The existing call to `sched_priority()` (which already happens on every
   tick for timeshare threads) is where the penalty is applied.
4. Inside `sched_priority()`, the cache-miss penalty is added to the
   computed priority, clamped so the result does not exceed `PRI_MAX_BATCH`.

### Step-by-step: PMC counter management

**PMC allocation:** The scheduler does NOT allocate or manage PMC hardware
counters itself.  Instead, it reads the hardware PMC register directly using
the machine-dependent `rdpmc()` intrinsic (on amd64) or the equivalent on
other architectures.  This is possible because:

- The LLC-miss event uses a fixed-function or well-known architectural PMC
  (Intel: event 0x2E, umask 0x41 for LONGEST_LAT_CACHE.MISS; AMD: event
  0x0762 for L3 miss; ARM: event 0x37 LL_CACHE_MISS_RD).
- The scheduler programs this counter once at boot (or when the feature is
  enabled at runtime) using the `pcd_write_pmc` / `pcd_start_pmc` class
  methods, allocating a system-wide counting PMC on each CPU.

**Alternative (simpler) approach -- adopted:** Rather than having the
scheduler directly manage PMC hardware, the scheduler relies on a dedicated
kernel subsystem (`sched_pmc`) that:

1. When `cachemiss_enabled` transitions from 0 to 1, allocates a system-wide
   counting-mode PMC on each online CPU, configured for the
   architecture-appropriate LLC-miss event.
2. Exposes a function `sched_pmc_read(int cpu)` that returns the current raw
   counter value for the LLC-miss PMC on the specified CPU.  This function
   calls through `pcd_read_pmc()` with interrupts disabled and is safe to
   call from the scheduler tick context (spin-lock held, no sleeping).
3. When `cachemiss_enabled` transitions from 1 to 0, releases the PMCs.

This subsystem is implemented in a new file `sys/kern/sched_pmc.c`,
compiled only when `HWPMC_HOOKS` is defined.

#### `sched_pmc_read(int cpu)` specification

```c
uint64_t sched_pmc_read(int cpu);
```

- **Preconditions:** Called with the current CPU's `tdq` lock held (spin
  lock context).  `cpu` must equal `curcpu` (the function reads the local
  CPU's PMC).  `cachemiss_enabled` must be 1.
- **Returns:** The current raw value of the LLC-miss counter for the
  calling CPU.  If hwpmc is not loaded or the PMC could not be allocated,
  returns 0.
- **Side effects:** None.  The counter is not reset; it free-runs.
- **Cost:** A single `rdpmc` instruction on x86 (tens of nanoseconds).

#### `sched_pmc_is_active(void)` specification

```c
int sched_pmc_is_active(void);
```

- **Returns:** 1 if the cache-miss PMC subsystem is active and counters are
  running; 0 otherwise.
- **Safe to call from any context** (reads a single `int` flag).

### Step-by-step: Counter reading in `sched_ule_clock()`

The following logic is added to `sched_ule_clock()`, after the existing
`sched_pctcpu_update(ts, 1)` call and before the priority recalculation,
guarded by `#ifdef HWPMC_HOOKS`:

```
1. If cachemiss_enabled == 0 or sched_pmc_is_active() == 0:
      Skip entirely (fast path for disabled case -- single branch).

2. ts->ts_cachemiss_ticks += cnt;

3. If ts->ts_cachemiss_ticks < cachemiss_interval:
      Skip PMC read (not time yet).

4. Reset ts->ts_cachemiss_ticks = 0.

5. Read the current counter:
      raw = sched_pmc_read(cpuid);

6. Compute delta:
      If ts->ts_cachemiss_prev == 0 (first read for this thread on this CPU):
          delta = 0   (no valid baseline yet)
      Else:
          delta = raw - ts->ts_cachemiss_prev
          (Counter overflow is handled by unsigned wraparound; the delta
           is correct for up to 2^64 - 1 misses between reads.)

7. Store current value:
      ts->ts_cachemiss_prev = raw

8. Accumulate:
      ts->ts_cachemiss += delta
```

**Counter reset on context switch in:** When a thread is switched in, its
`ts_cachemiss_prev` must be refreshed so that the next delta calculation
does not include misses from other threads.  This is done by adding a
small hook in `sched_ule_sswitch()` (in the post-`cpu_switch()` path
where the new thread resumes), guarded by `#ifdef HWPMC_HOOKS`:

```
If cachemiss_enabled and sched_pmc_is_active():
    ts->ts_cachemiss_prev = sched_pmc_read(cpuid);
```

This ensures the baseline is always set to the counter value at the
moment the thread starts running.

### Step-by-step: Penalty calculation in `sched_priority()`

The penalty is applied only to non-interactive timeshare threads (the
`score >= sched_interact` branch in `sched_priority()`).  Interactive
threads (those with a score below `sched_interact`) are not penalized,
as the intent is to keep interactive responsiveness intact.

In the non-interactive branch, after computing `pri` as
`PRI_MIN_BATCH + cpu_pri_off + nice_pri_off`, the following is inserted,
guarded by `#ifdef HWPMC_HOOKS`:

```
If cachemiss_enabled != 0:
    penalty = ts->ts_cachemiss_penalty
    pri = min(pri + penalty, PRI_MAX_BATCH)
```

The `ts_cachemiss_penalty` field is computed in `sched_ule_clock()` (not in
`sched_priority()`) to keep the priority function cheap:

```
After step 8 (accumulation) above:

9. If ts->ts_cachemiss > cachemiss_threshold:
       excess = ts->ts_cachemiss - cachemiss_threshold
       /* Scale: every 'cachemiss_threshold' additional misses adds 1 penalty level. */
       raw_penalty = (int)(excess / cachemiss_threshold)
       raw_penalty = raw_penalty * cachemiss_weight
       ts->ts_cachemiss_penalty = min(raw_penalty, SCHED_CACHEMISS_PENALTY_MAX)
   Else:
       ts->ts_cachemiss_penalty = 0
```

### Step-by-step: Window decay

The cache-miss accumulator (`ts_cachemiss`) must decay over time so that a
thread that stops causing cache misses eventually recovers its priority.
This decay is tied to the existing interactivity window mechanism.

In `sched_interact_update()`, when the runtime/slptime history is scaled
down, the cache-miss accumulator is also scaled down proportionally.
Specifically:

- When `sum > SCHED_SLP_RUN_MAX * 2` (the aggressive reset path):
  `ts->ts_cachemiss = 0`
- When `sum > (SCHED_SLP_RUN_MAX / 5) * 6` (the halving path):
  `ts->ts_cachemiss /= 2`
- In the normal 4/5 scaling path:
  `ts->ts_cachemiss = (ts->ts_cachemiss / 5) * 4`

After any of these scalings, `ts->ts_cachemiss_penalty` is recomputed
using the formula from step 9 above.

### Step-by-step: Thread lifecycle

**Thread creation (`sched_ule_fork_thread()`):**
- `ts_cachemiss` is initialized to 0.
- `ts_cachemiss_prev` is initialized to 0.
- `ts_cachemiss_ticks` is initialized to 0.
- `ts_cachemiss_penalty` is initialized to 0.

Cache-miss history is NOT inherited from the parent.

**Thread exit (`sched_ule_exit_thread()`):**
- No special cleanup needed; the fields are part of `td_sched` which is
  freed with the thread.

**Thread sleep/wakeup:**
- On wakeup (`sched_ule_wakeup()`), `ts_cachemiss_prev` is set to 0
  so that the first read after waking computes no delta (the counter
  may have advanced due to other threads while this thread slept).
  `ts_cachemiss_ticks` is also reset to 0.
- The accumulated `ts_cachemiss` and `ts_cachemiss_penalty` are preserved
  across sleep/wakeup so that a thread that repeatedly sleeps briefly
  and wakes to cause cache misses is still penalized.

---

## Edge Cases & Error Handling

### E1: hwpmc(4) module not loaded

When `cachemiss_enabled` is set to 1 but the hwpmc kernel module is not
loaded (i.e., `pmc_hook == NULL`), `sched_pmc_is_active()` returns 0.
The scheduler skips all PMC reads and applies no penalty.  No error is
reported; the sysctl write succeeds.  If hwpmc is later loaded and the
sysctl is toggled 0->1, the PMCs are allocated at that point.

### E2: PMC allocation failure

If the system has no available PMC hardware counters (all are in use by
userland pmcstat(8) or other consumers), `sched_pmc` fails to allocate.
`sched_pmc_is_active()` returns 0.  The scheduler operates normally
without any penalty.  A message is logged via `log(LOG_WARNING, ...)`.

### E3: Counter overflow / wraparound

PMC counters on x86 are typically 48 bits wide.  The delta computation
uses unsigned 64-bit subtraction, which correctly handles wraparound:
`delta = (uint64_t)(raw - ts->ts_cachemiss_prev)`.  If the counter wraps
around more than once between reads (which would require 2^48 LLC misses
in a single sampling interval -- physically impossible), the delta would
be incorrect, but this case cannot occur in practice.

### E4: Thread migration between CPUs

LLC-miss counters are per-CPU.  When a thread migrates:

- The context-switch-in path resets `ts_cachemiss_prev` to the new CPU's
  counter value.
- The delta from the partial tick on the old CPU is lost.  This is
  acceptable: the lost delta is at most one sampling interval's worth
  of misses, and the penalty is computed from accumulated misses, so
  losing one sample has minimal impact.

### E5: System under low load

When the system is lightly loaded and there is no CPU contention, the
penalty has no practical effect -- the thread runs regardless of its
priority because no other thread is competing for the CPU.  This is
correct and expected behavior.

### E6: Realtime and idle-class threads

Only timeshare-class threads (`PRI_BASE(td->td_pri_class) == PRI_TIMESHARE`)
are affected.  Realtime, interrupt, and idle threads are never penalized
by cache-miss data.  This is enforced by the existing `PRI_BASE` check at
the top of `sched_priority()`.

### E7: Feature toggled at runtime

When `cachemiss_enabled` transitions from 1 to 0:
- The PMCs are released (stopped and deallocated).
- All threads' `ts_cachemiss_penalty` fields remain at their current
  values temporarily.  They will naturally decay to 0 as
  `sched_interact_update()` scales down `ts_cachemiss` over subsequent
  ticks.

When `cachemiss_enabled` transitions from 0 to 1:
- PMCs are allocated and started.
- All threads begin with `ts_cachemiss_prev == 0`, so the first sample
  after enabling produces no delta.  Meaningful data begins accumulating
  after the second sample.

### E8: Non-x86 architectures

On architectures where `rdpmc()` is not available or where the PMC
subsystem does not support system-wide counting mode, the
`sched_pmc_init()` function returns an error and
`sched_pmc_is_active()` remains 0.  The feature is inert on such
platforms.  The sysctl knobs are still visible but have no effect.

### E9: Interaction with nice(1)

The cache-miss penalty is additive with the nice-based priority offset.
A thread with `nice 19` and maximum cache-miss penalty receives:
`PRI_MIN_BATCH + cpu_pri_off + SCHED_PRI_NICE(19) + SCHED_CACHEMISS_PENALTY_MAX`,
clamped to `PRI_MAX_BATCH`.  The clamping ensures the thread never falls
into the idle priority band.

### E10: Interaction with priority lending (turnstile/PI)

The cache-miss penalty applies only to the base user priority
(`td_base_user_pri`), which is set via `sched_user_prio()`.  Priority
lending (`td_lend_user_pri`) operates independently and takes precedence
when the lent priority is higher (lower numeric value).

---

## Invariants & Constraints

**INV-1:** The cache-miss penalty never causes a timeshare thread's
priority to exceed `PRI_MAX_BATCH` (223).  The penalty is always clamped:
`pri = min(pri + penalty, PRI_MAX_BATCH)`.

**INV-2:** `ts_cachemiss_penalty` is always in the range
`[0, SCHED_CACHEMISS_PENALTY_MAX]` (i.e., `[0, 50]`).

**INV-3:** When `cachemiss_enabled == 0`, no PMC reads occur and no
penalty is applied.  The fast-path check is a single load-and-branch on a
`__read_mostly` variable.

**INV-4:** The feature does not alter behavior when `HWPMC_HOOKS` is not
defined in the kernel configuration.  All new code is guarded by
`#ifdef HWPMC_HOOKS`.

**INV-5:** The feature never allocates memory, sleeps, or acquires a
sleeping lock from the scheduler tick or context-switch paths.

**INV-6:** `ts_cachemiss_prev` is always reset to the current CPU's counter
value when a thread is switched in, so that cross-CPU counter differences
never contaminate the delta calculation.

**INV-7:** The `thread0_storage.t0st_sched` static assertion continues to
hold after adding the new fields.

**INV-8:** PMC reads occur at most once every `cachemiss_interval` ticks
per thread (default 4).  At 127 Hz stathz, this is roughly every 31 ms.

**INV-9:** The cache-miss accumulator (`ts_cachemiss`) decays in lockstep
with the existing interactivity history, ensuring that past behavior does
not permanently penalize a thread.

---

## Performance Considerations

**Cost of `rdpmc`:** On modern x86, `rdpmc` takes approximately 20-40
cycles.  At the default `cachemiss_interval` of 4 ticks (one read every
~31 ms), this adds approximately 0.0001% overhead per CPU.

**Branch prediction:** The fast-path check (`if (cachemiss_enabled == 0)`)
on a `__read_mostly` variable will be well-predicted as not-taken when the
feature is disabled.

**Cache footprint:** The four new fields in `td_sched` add 24 bytes per
thread.  On a system with 10,000 threads, this is 240 KB of additional
memory -- negligible.

**Lock contention:** No additional locks are taken.  The PMC read is a
CPU-local operation performed under the existing `tdq` spin lock.

---

## Files Modified

| File | Change |
|---|---|
| `sys/kern/sched_ule.c` | Add `td_sched` fields, modify `sched_ule_clock()`, `sched_priority()`, `sched_ule_sswitch()`, `sched_ule_fork_thread()`, `sched_ule_wakeup()`, `sched_interact_update()`. Add sysctl knobs. |
| `sys/kern/sched_pmc.c` | **New file.** PMC lifecycle management for the scheduler: allocate/release per-CPU LLC-miss counters, provide `sched_pmc_read()` and `sched_pmc_is_active()`. Compiled only with `HWPMC_HOOKS`. |
| `sys/kern/sched_pmc.h` | **New file.** Header declaring `sched_pmc_read()`, `sched_pmc_is_active()`, `sched_pmc_init()`, `sched_pmc_fini()`. |
| `sys/conf/files` | Add `kern/sched_pmc.c` compilation rule, conditional on `HWPMC_HOOKS`. |
| `sys/sys/proc.h` | No changes (the `thread0_storage` size is sufficient). |

---

## Acceptance Criteria

### Compile-time

**AC-1:** A kernel built without `options HWPMC_HOOKS` compiles and boots
with no reference to any cache-miss-related code or symbols.  The
`struct td_sched` does not contain the new fields.

**AC-2:** A kernel built with `options HWPMC_HOOKS` compiles and boots
successfully.  The `_Static_assert` on `thread0_storage` size passes.

### Sysctl interface

**AC-3:** On a kernel with `HWPMC_HOOKS`, the four new sysctl knobs are
visible under `kern.sched.ule`:
`cachemiss_enabled`, `cachemiss_weight`, `cachemiss_threshold`,
`cachemiss_interval`.

**AC-4:** Default values match the specification: `cachemiss_enabled=0`,
`cachemiss_weight=1`, `cachemiss_threshold=1000`,
`cachemiss_interval=4`.

**AC-5:** Setting `cachemiss_weight` to a value > 10 results in it being
clamped to 10.  Setting it to a value < 0 results in it being clamped
to 0.

**AC-6:** Setting `cachemiss_interval` to a value > 100 results in it
being clamped to 100.  Setting it to < 1 results in it being clamped to 1.

### Disabled behavior

**AC-7:** With `cachemiss_enabled=0`, running a cache-miss-heavy workload
(e.g., random-access traversal of a large array) alongside a cache-friendly
workload shows no change in scheduling priority for either thread compared
to an unmodified kernel.

### Enabled behavior

**AC-8:** With `cachemiss_enabled=1`, `cachemiss_weight=1`,
`cachemiss_threshold=1000`: a thread that consistently generates LLC misses
well above the threshold has a higher (worse) `td_base_user_pri` than a
CPU-bound thread of the same nice value that fits in cache.  Specifically,
`sysctl kern.proc.all` or equivalent inspection shows the cache-miss-heavy
thread with a priority at least 1 level worse (higher numeric value).

**AC-9:** The priority penalty for a cache-miss-heavy thread never exceeds
`SCHED_CACHEMISS_PENALTY_MAX` (50) priority levels, regardless of how many
misses occur.

**AC-10:** The priority penalty for a cache-miss-heavy thread never pushes
the thread's priority below `PRI_MAX_BATCH` (numeric 223) -- it must not
enter the idle priority band.

### Priority recovery

**AC-11:** After a cache-miss-heavy thread changes its access pattern to
become cache-friendly, its `ts_cachemiss_penalty` decays to 0 within
`SCHED_SLP_RUN_MAX` seconds (approximately 5 seconds at default settings).

### Thread lifecycle

**AC-12:** A newly forked thread starts with `ts_cachemiss == 0` and
`ts_cachemiss_penalty == 0`, regardless of the parent's cache-miss history.

**AC-13:** A thread that sleeps and then wakes up retains its accumulated
`ts_cachemiss` but resets `ts_cachemiss_prev` so the first PMC read after
waking does not produce a spurious delta.

### CPU migration

**AC-14:** When a thread migrates from CPU A to CPU B, the first cache-miss
delta computed on CPU B is zero (because `ts_cachemiss_prev` is reset to
CPU B's counter value on switch-in).

### hwpmc not loaded

**AC-15:** With `cachemiss_enabled=1` but the hwpmc kernel module not
loaded, the scheduler does not panic, does not print errors to the console
on every tick, and applies no cache-miss penalty.  A single warning message
is logged when the enable is attempted.

### Interaction with nice

**AC-16:** A thread with `nice 0` and maximum cache-miss penalty has a
lower scheduling priority (higher numeric value) than a thread with
`nice 0` and zero cache-miss penalty, all else being equal.

**AC-17:** A thread with `nice -20` is not penalized by cache-miss data
if it qualifies as interactive (score < `sched_interact`).

### Weight control

**AC-18:** Setting `cachemiss_weight=0` with `cachemiss_enabled=1` results
in no priority penalty being applied, even for threads with high cache-miss
counts.

**AC-19:** Setting `cachemiss_weight=5` results in a penalty 5x larger
than `cachemiss_weight=1` for the same cache-miss rate, up to the
`SCHED_CACHEMISS_PENALTY_MAX` cap.

### Runtime toggle

**AC-20:** Toggling `cachemiss_enabled` from 1 to 0 releases the PMC
resources.  Subsequent PMC allocation by userland pmcstat(8) for the same
hardware counter succeeds.

**AC-21:** Toggling `cachemiss_enabled` from 0 to 1 while the PMC hardware
counters are all in use by other consumers results in no crash and a
warning message.  `sched_pmc_is_active()` returns 0.

---

## Out of Scope

- **L1/L2 cache miss tracking:** This spec covers LLC (last-level cache)
  misses only.  Extending to other cache levels is future work.

- **Per-process (as opposed to per-thread) penalty:** The penalty is
  per-thread.  Aggregating across all threads in a process is not covered.

- **Userland visibility of per-thread cache-miss data:** Exposing
  `ts_cachemiss` or `ts_cachemiss_penalty` via `procstat(1)` or similar
  tools is future work, though the data structures support it.

- **Automatic tuning of thresholds:** The threshold and weight values are
  static sysctl knobs.  Adaptive algorithms are out of scope.

- **Non-timeshare scheduling classes:** Realtime, interrupt-thread, and
  idle-class threads are never affected.

- **IBS (Instruction-Based Sampling) on AMD:** Only counting-mode PMCs are
  used; sampling-mode / IBS integration is out of scope.

- **NUMA-aware penalty adjustment:** The penalty does not distinguish between
  local and remote memory access patterns.

- **Dynamic PMC event selection:** The LLC-miss event is hardcoded per
  architecture.  Allowing the administrator to select an arbitrary PMC
  event is out of scope.
