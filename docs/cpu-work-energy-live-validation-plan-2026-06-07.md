# CPU Work/Energy Live MSR Validation Plan - 2026-06-07

Status: **executed** procedure. This plan ran 2026-06-07 — outcome **PASS
(energy-only)**; results in
`docs/cpu-work-energy-live-validation-results-2026-06-07.md`. The procedure body
below is retained as the method of record. It defined the one-shot, read-only
live MSR validation that
`docs/features/FEAT-0006-cpu-work-energy-efficiency-evidence.md` §13 named as the
**pre-implementation gate** between FEAT-0006 (Accepted) and energy-only build
authorization. It was read-only and one-shot; per `AGENTS.md` §Live Runtime
Safety the operator authorized the live read session, and the maintainer
authorized the energy-only implementation afterward.

The design direction is already settled and is **not** re-opened here. The
acquisition path, allow-list, safety review, and derivation are normative in
`docs/cpu-work-energy-acquisition-decision-2026-06-07.md`; the feasibility
findings and the four open live questions are in
`docs/cpu-work-energy-acquisition-verification-2026-06-04.md` §Follow-up. This
plan only operationalizes how to answer those four questions on the target part.

**Companion to:**
`docs/features/FEAT-0006-cpu-work-energy-efficiency-evidence.md`,
`docs/cpu-work-energy-acquisition-decision-2026-06-07.md` (settled design — read
its §Decision, §Counter-read safety review, §Derivation, §Apply order),
`docs/cpu-work-energy-acquisition-verification-2026-06-04.md` (feasibility input
and the four open live items),
`AGENTS.md` §Live Runtime Safety / §Repo Boundary,
`src/hardware/amd_reader.cpp` (`ExecutePawnIo`, the `Global\Access_PCI` section),
`src/hardware/pawnio_binary.cpp` (`kPawnIoSpecAmdFamily17V1`, the `warn_only` load).

## 1. Scope

**In scope.** A single supervised, read-only session on the development part
(AMD Ryzen 9 9950X3D, Family 1Ah / Zen 5, model `0x44`) that answers the four
open live questions and produces one outcome:
`PASS (energy+frequency)` / `PASS (energy-only)` / `FAIL` / `BLOCKER`.

**Out of scope (named so they are not confused with this gate).**

- This is **not** the post-implementation quarantine-exit Evaluation. That
  separate, later gate (≥ 3 capture sessions spanning ≥ 7 days, counter-continuity
  across a wrap, the ±15% external cross-check, the no-disturbance `loop_slip_ms`
  gate) is defined in `cpu-work-energy-acquisition-decision-2026-06-07.md`
  §Evaluation and §Quarantine and is **not** restated here. This plan's outcome
  authorizes *implementation*; it does **not** promote any data to `validated`.
- No production code lands in this gate. The reads run through a throwaway probe
  (§9), not the production MSR helper or the control loop.
- No CPU actuation, no fan write, no start/stop/breaker reset, no cadence/channel
  change, no `ioctl_write_msr` (`AGENTS.md` §Live Runtime Safety; REQ-CPUEFF-05).

## 2. What this gate must answer

These are the four open live items from the verification doc §Follow-up item 3,
each bound to a concrete check below.

| # | Open question | Settled by | Decides |
|---|---|---|---|
| Q1 | Does PawnIO honor caller `SetThreadAffinityMask` for `rdmsr`? | §6.3 | Whether effective frequency (APERF/MPERF) is trustworthy at all. |
| Q2 | Is AMD RAPL available, and is its energy unit encoded as documented, on Family 1Ah? | §6.1–§6.2 | Whether package energy is a usable v1 signal on this part. |
| Q3 | Do APERF/MPERF yield a plausible effective frequency under held affinity? | §6.4 | Whether effective frequency passes its own plausibility check (only if Q1 passes). |
| Q4 | Does an absent/unsupported MSR degrade to blank (no crash, no false zero)? | §6.5 | Whether the `#GP` path is safe before any code consumes it. |

## 3. Prerequisites

1. **Safety review done.** REQ-CPUEFF-06 is satisfied — the counter-read safety
   review is folded into `cpu-work-energy-acquisition-decision-2026-06-07.md`
   §Counter-read safety review. The live read is therefore no longer gated on
   more design work; it is gated only on operator authorization to run and the
   probe in §9.
2. **Explicit operator authorization** to perform a read-only live MSR session
   (`AGENTS.md` §Live Runtime Safety).
3. **Controller not driving fans during the session.** Run the probe with the
   live control loop stopped, or otherwise in a state where the probe is the only
   path opening a PawnIO handle, so the session cannot disturb live fan output
   and does not contend with the temperature read path. The probe must never
   write fan duty or reset breakers.
4. **The shipped transport.** `resources/pawnio/AMDFamily17.bin` loaded via the
   PawnIO driver, exactly as the controller loads it (`kPawnIoSpecAmdFamily17V1`,
   `warn_only`). No new or swapped bin.

## 4. Read sets used in this session

Two disjoint sets. Keep them distinct in the probe and in the results note.

**Production allow-list (the only MSRs the shipped helper will ever read).** This
set is fixed by the decision doc §Counter-read safety review and is exercised
here exactly as production will use it:

- `0xC0010299` `MSR_RAPL_PWR_UNIT` — energy unit (read once).
- `0xC001029B` `MSR_PKG_ENERGY_STAT` — package energy accumulator (32-bit).
- `0xE7` `IA32_MPERF`, `0xE8` `IA32_APERF` — reference / delivered cycles.

**Probe-only diagnostic read (must NOT enter the production allow-list).**

- `0xC0000103` `IA32_TSC_AUX` — read-only; the value the OS programs per logical
  processor (what `RDPID`/`RDTSCP` return). Used once in §6.3 as a direct
  per-read core-identity probe for the affinity question, then discarded. The
  production helper never reads it; it stays off the allow-list. It is included
  here only because Q1 cannot be answered with the package-scoped or
  ratio-only allow-list MSRs alone.

All reads go through `ioctl_read_msr`. `ioctl_write_msr` is never called.

## 5. Plausibility bounds (proposed; finalized at implementation)

Concrete values the probe uses to classify a reading. These are proposed
starting constants, not normative; the decision doc lists the exact socket
ceiling as an open item to fix at implementation against the measured idle floor.

- **Energy unit (ESU).** `0xC0010299` bits `[12:8]`. Expected default
  `0b10000` = 16 → `1/2^16` J = 15.26 µJ/unit (verification doc §Evidence). An
  ESU that decodes to `0` or to a unit implying sub-µJ or > mJ resolution is
  treated as an encoding mismatch and recorded as such (Q2 fail signal).
- **Socket power ceiling (implausibility guard, high-power branch).** Proposed
  **400 W**: above this 170 W-TDP-class socket's configured PPT and any PBO
  headroom (the decision doc's worked example treats ~200 W as a representative
  true package power), and far below the ~65 kW that a single sub-wrap (~1 s)
  window would require to multi-wrap (`2^32` units × 15.26 µJ ≈ 65.5 kJ ÷ ~1 s).
  Apparent average power above the ceiling ⇒ blank that window.
- **Window duration bound (implausibility guard, Δt branch).** Δt missing,
  non-positive, or exceeding `k ×` the ~1 s resource window (`k ≈ 2–3`, i.e. a
  few seconds) ⇒ blank that window. This is the load-bearing branch that keeps
  multi-wrap aliasing-to-low-power out (decision doc §Derivation).
- **Idle / load power expectation.** At idle, derived average package power is
  positive and well below the load-power figure §6.2 measures; under a sustained
  all-core load it rises toward the configured PPT and falls back when the load
  stops. Pinned constant regardless of load, negative, or above the ceiling ⇒
  Q2 fail.
- **Effective frequency band.** `(ΔAPERF/ΔMPERF) × reference` (reference =
  base/P0 nominal) lands between a deep-idle clock and the part's rated maximum
  boost. Values outside that band — e.g. an effective clock above the rated
  maximum boost, which is what a cross-core `ΔAPERF/ΔMPERF` ratio produces —
  indicate affinity was not held/honored (Q1/Q3 fail).

## 6. Procedure

Each step is read-only. Record raw values and the derived result for each.

### 6.1 RAPL units and encoding (Q2)

Read `0xC0010299` once. Record the raw 64-bit value and the decoded ESU. A
clean read with a plausible ESU (§5) is the Q2 precondition; a `#GP`/error here
means RAPL is unavailable on this part (Q2 = absent → see §7 FAIL).

### 6.2 Package energy plausibility (Q2)

With the ESU from §6.1, sample `0xC001029B` repeatedly over a short series that
spans: (a) ~30 s idle, (b) a sustained all-core load (e.g. a CPU stress run long
enough to be steady), (c) ~30 s cooldown. For each adjacent pair apply 32-bit
modular subtraction `(cur - prev) & 0xFFFFFFFF`, convert units → µJ, divide by
Δt for average watts. Confirm: monotonic counter (no negative deltas after
modular subtraction), idle power positive and well below the load figure, load
power rises and falls with the load and stays below the §5 ceiling. Record a compact summary
(idle/load/cooldown average watts and the raw ESU), not the raw per-sample CSV
(`AGENTS.md` §Live Runtime Safety).

> A full wrap (~5–6 min at ~200 W) is **not** required for this gate; spanning a
> wrap is a quarantine-exit criterion (decision §Evaluation §1), not a
> pre-implementation one.

### 6.3 Affinity-honoring test (Q1)

The crux question: does `ioctl_read_msr` execute `rdmsr` on the **calling**
thread (so caller-side `SetThreadAffinityMask` pins the read to the intended
core) or marshal it to PawnIO's own context (so caller affinity is ignored and
APERF/MPERF pairs may straddle cores → a meaningless ratio)?

**Primary — direct core identity via `IA32_TSC_AUX`.** Choose ≥ 3 logical
processors spread across both CCDs (e.g. one on CCD0, one on CCD1, one more).
For each chosen core C:

1. `SetThreadAffinityMask(GetCurrentThread(), 1 << C)`, then spin on
   `GetCurrentProcessorNumber()` until it equals C before reading — `Sleep(0)`
   alone does not guarantee the thread has landed on core C.
2. Establish the OS baseline on that same pinned thread: read the core identity
   directly via `RDPID`/`RDTSCP` (or `GetCurrentProcessorNumber()`).
3. Read `0xC0000103` through `ioctl_read_msr`.
4. Compare. **Affinity honored** iff the MSR-read value equals the same-thread
   `RDPID` baseline for **every** chosen core. **Affinity not honored** iff the
   MSR-read value is constant (or maps to one fixed core) while the `RDPID`
   baseline varies — PawnIO marshaled the read off the caller's core.

This comparison needs no decode of the OS's `TSC_AUX` packing; it only needs the
MSR-read value to track the same-thread `RDPID` value across cores.

**Corroboration — per-core APERF distinctness (allow-list only).** Under each
pin, read `0xE8` (APERF). Cores have independent free-running APERF since reset;
honored affinity yields distinct per-core values/rates, while marshaling yields
one core's stream regardless of the requested pin. This corroborates the primary
result using only allow-list MSRs, in case `0xC0000103` itself faults.

### 6.4 Effective-frequency plausibility (Q3 — only if §6.3 = honored)

Under held affinity to a single core, read the `0xE8`/`0xE7` pair twice across a
known interval; compute `(ΔAPERF/ΔMPERF) × reference`. Confirm the result is in
the §5 effective-frequency band and is stable while affinity is held, at idle and
under load. If §6.3 found affinity is **not** honored, skip this step and record
effective frequency as withheld.

### 6.5 `#GP` → blank fault behavior (Q4)

Read one MSR index known to be unsupported on this part (probe-only; not the
allow-list) and confirm `ioctl_read_msr` returns an error that the probe maps to
blank — never a crash, never a `0`. This exercises the degrade-to-blank path the
decision doc §Counter-read safety review requires before any field consumes it.
If RAPL was absent in §6.1, that read already exercised the same path.

## 7. Outcome decision tree

Combine the per-check results into exactly one outcome:

- **PASS (energy+frequency)** — Q2 pass (RAPL present, ESU plausible, power
  tracks load), Q1 = affinity honored, Q3 plausible, Q4 safe. Authorizes
  implementing the v1 energy **and** effective-frequency signals
  (REQ-CPUEFF-01..08); both ship in `quarantine` per the decision doc.
- **PASS (energy-only)** — Q2 pass and Q4 safe, but Q1 = affinity **not**
  honored (or Q3 implausible). Authorizes implementing the energy path only;
  APERF/MPERF cycles ship withheld (`cpu_cycles_acquisition` not `validated`),
  matching the decision doc's "energy-only" branch.
- **FAIL** — Q2 = RAPL absent or implausible on Family 1Ah. The v1 energy signal
  has no trustworthy source; do not implement the energy path as specced.
  Escalate to the maintainer — this is the "RAPL fails on this hardware" branch;
  energy stays indefinitely unpromotable and the deferred SMU/PPT source
  decision (decision §Options) would have to be revisited.
- **BLOCKER** — Q4 fails (a read crashes or returns a false zero instead of
  degrading to blank). Fix the degrade-to-blank handling in the probe and in the
  planned helper design before any outcome is recorded as PASS.

A PASS authorizes implementation **only after** the maintainer explicitly
authorizes build work (acquisition decision §"What this does not authorize").

## 8. Live Runtime Safety procedure

- Run only with explicit operator authorization, controller not driving fans
  (§3.3), operator-supervised, once.
- The probe opens its own PawnIO handle and issues only read functions
  (`ioctl_read_msr`), then exits. The handle is opened with `GENERIC_WRITE`
  because that access right is required to issue any IOCTL (including the bin
  load); it does **not** imply any MSR write. `rdmsr` does not use PCI config
  space, so the reads run outside the `Global\Access_PCI` section and do not
  extend its hold or contend with the SMN temperature path (decision
  §Disturbance mitigation §3).
- No `ioctl_write_msr`, no fan duty write, no start/stop/restart, no breaker
  reset, no scheduled-task install (`AGENTS.md` §Live Runtime Safety).

## 9. Throwaway probe harness — `tools/cpu_msr_validation_probe.cpp` (removed after the gate)

The reads needed a minimal tool. It was throwaway diagnostic tooling, **not** the
production MSR helper and **not** REQ-CPUEFF implementation. It was built,
compile-verified (MSVC `/W4 /permissive-`), and run once 2026-06-07; the source
and its `SVG_MB_CONTROL_BUILD_MSR_PROBE` build option were then removed (results
in `docs/cpu-work-energy-live-validation-results-2026-06-07.md`, Next steps). The
build and structure description below is the recorded method.

- **Build (off by default):**
  `cmake -S . -B <dir> -DSVG_MB_CONTROL_BUILD_MSR_PROBE=ON` then
  `cmake --build <dir> --target cpu_msr_validation_probe --config Release`. The
  `SVG_MB_CONTROL_BUILD_MSR_PROBE` option is OFF by default, so `Test-LocalCI` /
  `Build-Release` / CTest never build it and `release\` never ships it.
- **Off production code.** It reuses only the clean bin-load API
  (`pawnio_binary.h`: `ResolvePawnIoBinaryPath` / `LoadPawnIoBinary` /
  `kPawnIoSpecAmdFamily17V1`) and **replicates** the small read-only execute
  IOCTL — the same call shape as the existing `ioctl_read_smn` temperature read
  with a different function name. It does not modify `src/hardware`, does not
  de-anonymize `ExecutePawnIo`, and does not link `svg_mb_control_core`.
- **Structurally read-only.** It issues only `ioctl_read_msr`, gated on a
  hard-coded read allow-list = the production set (`0xC0010299`, `0xC001029B`,
  `0xE7`, `0xE8`) plus the probe-only `0xC0000103` for the affinity test; the
  PawnIO write entry point name does not appear in the file.
- It implements `SetThreadAffinityMask` pin + `GetCurrentProcessorNumber` spin
  for §6.3/§6.4, modular-subtraction energy math for §6.2, the §5 plausibility
  classification, and maps any IOCTL error to blank for §6.5; stdout maps 1:1 to
  the §10 result block plus the §7 outcome.
- Delete it after the gate is recorded. The production helper, allow-list, and
  field-level strict hash gate are built later under the decision doc §Apply
  order — after this gate passes and the maintainer authorizes the build.

## 10. Recording and what to update on completion

After the session, record a compact results note (not raw CSV) capturing: the
raw `0xC0010299` value and decoded ESU; the idle/load/cooldown average-watts
summary; the Q1 affinity verdict with its evidence (the per-core MSR-read vs
`RDPID` comparison); the Q3 effective-frequency sample (if taken); the Q4 fault
behavior; and the single §7 outcome.

Then, in the same change:

1. Add the results as a dated note —
   `docs/cpu-work-energy-live-validation-results-YYYY-MM-DD.md` — or append a
   "Live validation results" section to the verification doc, and flip its
   §Follow-up item 3 from "Remaining before implementation authorization" to
   done with the outcome.
2. Update FEAT-0006 §13's live-validation note and §14 context with the outcome,
   and (only when the maintainer authorizes the build) the
   `docs/TRACEABILITY.md` REQ-CPUEFF status from `not buildable`.
3. Leave promotion of any captured data to `validated` to the separate
   post-implementation quarantine-exit Evaluation (decision §Evaluation) — this
   gate does not perform it.
