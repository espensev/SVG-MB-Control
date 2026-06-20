# FEAT-0020 Critical Evaluation

**Archive status:** completed and archived 2026-06-20. This file is retained as
the pre-implementation review record; current FEAT-0020 status lives in
`docs/features/FEAT-0020-standard-control-loop-power-logging.md` and
`docs/TRACEABILITY.md`.

**Project:** svg-mb-control
**Status:** Review note; reconciled into FEAT-0020 v0.2 and D-PWRLOG-1
**Updated:** 2026-06-18
**Reviewed docs:** `docs/features/FEAT-0020-standard-control-loop-power-logging.md`,
`docs/power-logging-flip-plan-2026-06-18.md`,
`docs/archive/implemented-plans/feat-0020-power-logging-implementation-plan-2026-06-18.md`,
`docs/as-is-logging-opportunities-2026-06-18.md`,
`docs/logging-next-targets-2026-06-18.md`,
`docs/features/FEAT-0021-standard-control-loop-gpu-workload-context-logging.md`

## Verdict

The direction is good, but the work is not ready for product code. The early
specs captured the right boundary: logging-only, additive fields, no control-law
change, no external bridge. The later FEAT-0020 implementation plan correctly
found several high-risk gaps. The problem now is governance drift: the detailed
implementation plan has outpaced the governing FEAT-0020 spec and D-PWRLOG-1
decision record.

Resolution update: this review was applied to FEAT-0020 v0.2 and D-PWRLOG-1 on
2026-06-18. Use `docs/archive/implemented-plans/feat-0020-power-logging-implementation-plan-2026-06-18.md`
for the current work split, implementation status, and stop points. This note
remains as the critical record of the issues that had to be reconciled.

## Findings

### F1 - Decision state is contradictory

`FEAT-0020` still says D-PWRLOG-1 is `Proposed`, and its open decision defaults
to a bounded cached GPU read unless per-tick is proven cheap. The implementation
plan says "DECIDED 2026-06-18" and "Maintainer decision" for a 5-field,
per-tick-by-default, cadence-agnostic schema.

Evidence:

- `docs/features/FEAT-0020-standard-control-loop-power-logging.md` §9 marks
  D-PWRLOG-1 `Proposed`.
- `docs/features/FEAT-0020-standard-control-loop-power-logging.md` §11 still
  prefers bounded cached sampling by default.
- `docs/archive/implemented-plans/feat-0020-power-logging-implementation-plan-2026-06-18.md` §3.1 marks
  the 5-field schema as decided.

Impact: a future implementer has two conflicting sources of truth. The registry
and traceability correctly mark the feature not buildable, so the "DECIDED"
language in the implementation plan is too strong until D-PWRLOG-1 is updated
and marked Current.

Required fix: either promote D-PWRLOG-1 to Current with the 5-field/per-tick
decision, or downgrade the implementation plan wording from "DECIDED" to
"recommended".

### F2 - The safety-revert path is more invasive than the current spec implies

The spec says the operator workflow must account for
`scripts/Reset-EnergyToDisabled.ps1`, but the implementation plan correctly
shows this is not a passive environment-variable clobber. The script forces the
User env vars to disabled and, when it sees a live `quarantine` marker, stops
the scheduled task, kills `svg-mb-control.exe`, and starts the task again.

Evidence:

- `scripts/Reset-EnergyToDisabled.ps1` writes both FEAT-0006 env vars to
  `disabled`.
- The same script calls `Stop-ScheduledTask`, `Stop-Process -Force`, and
  `Start-ScheduledTask` when a quarantine marker is observed.

Impact: a simple standard-profile env flip can be torn down at boot/logon. This
is a governance change to FEAT-0006's "boot/logon returns energy to disabled"
safety premise, not just an operator convenience.

Required fix: D-PWRLOG-1 must explicitly choose how the standard logging profile
coexists with the safety revert. The implementation plan's
`Set-EnergyLoggingProfile.ps1 -Enable/-Disable` pair is plausible, but it is not
yet accepted.

### F3 - The analyzer work is under-scoped in the spec

`FEAT-0020` says analyzer ingest/reporting should bind fields by header name and
continue ingesting old archives. That is necessary but incomplete. The current
analyzer parses CSV fields by name, but the DB ingest layer uses a hardcoded
positional `INSERT INTO tick_samples(...)` with binds through slot 41, and the
database is versioned at schema 10.

Evidence:

- `src/analyze/analyze_csv.cpp` does name-based CSV field lookup.
- `src/analyze/analyze_ingest_db.cpp` has a positional `INSERT INTO
  tick_samples(...)` and bind calls through `BindOptionalText(41, ...)`.
- `src/analyze/analyze_db.h` defines `kSchemaVersion = 10`.

Impact: GPU power is not a small parser-only change. It requires a v10-to-v11
schema migration, fresh DB schema changes, positional insert/bind updates,
report data/emit support, and old-archive degradation tests.

Required fix: update FEAT-0020 §5/§7/§10 and traceability wording so
REQ-PWRLOG-05 explicitly covers the DB migration and positional ingest layer.

### F4 - GPU power math must be locked before analyzer work

The implementation plan correctly separates GPU power from CPU package energy.
CPU power is derived from an energy delta over a time window. GPU
`nvml_power_mw` is already a milliwatt sample for total board power, not an
accumulating energy counter.

Evidence:

- `third_party/nvapi-controller/telemetry/include/gpu_telemetry/gpu_snapshot.h`
  stores `nvml_power_mw` and comments that total board power should use that
  field.
- `third_party/nvapi-controller/src/nvml_loader.cpp` calls
  `nvmlDeviceGetPowerUsage`.
- CPU power analyzer code groups by `cpu_power_sample_id` because the CSV
  mirrors one energy window across multiple ticks; this is not the same signal
  shape as instantaneous GPU milliwatts.

Impact: if the analyzer copies the CPU energy pattern, it can produce a
plausible but wrong GPU number.

Required fix: FEAT-0020 should require GPU power summaries as sample statistics
such as mean/p50/p90/max over present GPU power samples, with de-duplication
only if the final cadence uses cached repeated samples.

### F5 - Live marker semantics need sharper wording

`FEAT-0020` currently lists disabled, unavailable, quarantine, and validated as
states to preserve. Runtime docs allow `validated`, but source makes the
important distinction: the live worker never auto-promotes to `validated`;
`validated` is a post-hoc evaluation/promotion state.

Evidence:

- `src/hardware/amd_reader.cpp` comments that the energy path never
  auto-promotes to `validated`.
- The live worker assigns `disabled`, `unavailable`, or `quarantine`.
- `docs/RUNTIME_HOME.md` already says `validated` is set only after
  post-implementation Evaluation and never automatic.

Impact: the current FEAT-0020 wording can be read as if the standard live
profile should produce `validated`. That would be wrong.

Required fix: update FEAT-0020 and `docs/RUNTIME_LOGGING_AND_EVALUATION.md` to
separate live producer states from post-hoc evaluation states.

### F6 - Gate 6 is structurally awkward

`FEAT-0020` says every promotion gate must pass before the feature is buildable,
but REQ-PWRLOG-04 is an M/R runtime measurement that can only close after code
exists and a separately authorized live flip runs. The implementation plan
correctly calls this out, but the governing spec still presents gate 6 as a
pre-build blocker.

Impact: taken literally, FEAT-0020 blocks itself. Taken loosely, it weakens the
spec gate. Neither is good.

Required fix: choose one of two clean paths:

- two-phase acceptance: build-authorize after gates 1-5 and 7, then close gate 6
  only after live flip evidence; or
- pre-build probe: add a throwaway/default-off GPU power latency probe and use
  it to satisfy a pre-implementation measurement gate before building the
  standard CSV path.

Do not leave the current ambiguity in place.

### F7 - One implementation-plan edit site is wrong

The implementation plan says the `RuntimeSnapshot` GPU struct is in
`runtime_artifacts.h`. The actual `RuntimeGpuSnapshot` and `RuntimeSnapshot`
types are in `src/runtime/runtime_snapshot.h`, and `MergeGpuTelemetry` in
`src/platform/direct_runtime_snapshot.cpp` currently copies only GPU
temperature/name/warning fields.

Impact: low immediate risk, but this is exactly the kind of wrong file pointer
that burns time during implementation or causes a partial patch.

Required fix: correct the implementation plan before anyone starts code:
runtime GPU power fields belong in `src/runtime/runtime_snapshot.h` and are
plumbed through `src/platform/direct_runtime_snapshot.cpp`.

### F8 - FEAT-0021 is acceptable as a held draft, but should not move yet

FEAT-0021 is properly held behind FEAT-0020 and does not authorize code. Its
scope is reasonable: GPU utilization, clocks, pstate, and VRAM beside GPU power.
But it depends on unresolved FEAT-0020 decisions: exact sample cadence, field
identity, acquisition marker values, and analyzer schema shape.

Impact: building FEAT-0021 before FEAT-0020 is settled would multiply schema and
measurement risk.

Required fix: no product-code work for FEAT-0021. After FEAT-0020's GPU sample
surface is current, revisit FEAT-0021 and either fold it into a v2 revision or
keep it as a separate feature.

## What is good

- The core product boundary is right: power and GPU context are logging-only.
- The no-external-bridge rule is preserved.
- The CPU side correctly reuses FEAT-0006 instead of inventing another CPU watts
  path.
- The 5-field GPU power schema is defensible because it preserves sample
  identity if cadence changes later.
- The implementation plan correctly spots the biggest hidden work:
  analyzer schema migration, instantaneous GPU power math, and the active safety
  revert.

## Required next action

Before code:

1. Update D-PWRLOG-1 and FEAT-0020 so there is one current decision on cadence,
   field set, acquisition marker values, analyzer math, and safety-revert
   governance.
2. Correct the `runtime_artifacts.h` edit-site error in the implementation plan.
3. Decide the gate-6 lifecycle model: two-phase post-build runtime verification
   or pre-build latency probe.
4. Re-run `python -m unittest tests.test_feature_specs -v`.

Only after that should FEAT-0020 be considered for implementation
authorization.
