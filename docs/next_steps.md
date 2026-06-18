# Review-Derived Next Steps

## Status

Reviewed and rewritten 2026-06-09; FEAT-0008 status refreshed 2026-06-16; the
write-path safety review (FEAT-0010–0014) was **closed 2026-06-17** (see below).
These are review notes and a backlog, not work
authorization: per `AGENTS.md` (Feature Intake Gate), product-code work for a new
capability, schema/status/log field, CLI surface, or shipped-config behavior must
go through an implementation-authorized `docs\features\FEAT-*` spec first. Each
item below names the governing doc or spec.

This supersedes the earlier agent-generated list. That list was checked item by
item against the current repo; the correction is recorded under "Prior list" so
the reasoning is auditable.

## Grounded backlog

### Write-path safety review (FEAT-0010–0014) — CLOSED 2026-06-17

The 2026-06-17 external review of the control/write safety path (handoff in
`Handoff/`) was worked to its acceptance bar and **closed**. All blocking and
recommended-decidable findings are implemented, merged, and CI-verified on `main`;
each is `Implemented` with a current decision record and `REQ-*` rows in
`docs\TRACEABILITY.md` (governance `test_feature_specs` 5/5):

- **FEAT-0010** — a sidecar-persist fault no longer vetoes the fan write (finding
  H1); v0.3 added `REQ-WRITESAFE-06` (failure-path event logging is best-effort /
  non-vetoing).
- **FEAT-0011** — bounded half-open breaker probe (one write per 5 s on rising
  cooling demand) so a recovered actuator self-heals (finding 5).
- **FEAT-0012** — a corrupt `pending_writes.json` is quarantined to
  `pending_writes.json.corrupt` and startup proceeds instead of relaunch-thrashing
  (finding H2).
- **FEAT-0013** — a CPU-input dropout on a `max_cpu_gpu_source_aware` channel now
  trips the existing safe mode instead of being masked by the GPU fallback (finding
  #1); all six live channels were affected.

Remaining, **all non-blocking** (backlog, not authorized work):

- **FEAT-0014** (`REQ-RESTOREGUARD-*`, Draft/held) — reconcile/restore does not
  consult the blocked-channel runtime policy. A blocked-channel sidecar entry is
  unreachable under the shipped single-profile config and the fail direction is
  bounded/one-shot; promote only if a multi-profile config makes it reachable.
- **Live (M) validation on hardware** — none of the four was runtime-reproduced on
  the box (the review required C++ tests, which are met). Recommended next
  confidence tier, especially FEAT-0013 (drop the real CPU sensor, observe safe
  mode) and FEAT-0012 (corrupt a live sidecar, observe the quarantine break the
  thrash loop).
- **Linux-only CI nicety** — `tests/test_eval_dashboard.py` `…server_help` should
  skip when neither `powershell` nor `pwsh` is present (only fails on non-Windows
  review runners; the shipped CI is Windows). Trivial.
- **Known scoped residuals** (recorded, not defects): legacy `MaxCpuGpu` still has
  the CPU-dropout masking gap FEAT-0013 fixed for source-aware (unused live);
  FEAT-0013 safe mode is the existing rate-limited ramp to 100%, not an instant
  jump; FEAT-0012 health stays degraded until `pending_writes.json.corrupt` is
  removed (deliberate — so the lost-records signal is acknowledged).

### Require fresh runtime evidence before changing cadence/floor defaults
The shipped profile is a `250 ms` control tick and `250 ms` write cooldown. Per
`README.md` (Documentation) and `docs\RUNTIME_LOGGING_AND_EVALUATION.md`, lowering
cadence, enabling a floor below the shipped profile, adding live channels, or
broader strategy changes must be backed by fresh measurement evidence before the
default changes.

The mechanism is the evaluation workflow, not `Test-LocalCI.ps1`:
`Test-LocalCI.ps1` runs `Build-Release.ps1 -NoStopProcesses -NoPublish` (a
hermetic build plus unit/smoke lane) and produces no runtime CSV. Collect runtime
evidence instead with a live control-loop capture followed by
`svg-mb-control analyze ingest` + `analyze report`, and/or
`scripts\Compare-CpuTemps.ps1` for the CPU-temperature-by-load view
(`docs\cpu-temp-comparison-harness.md`).

### FEAT-0006 (CPU work/energy) downstream work
The spec is already `Accepted` (`docs\features\FEAT-0006-cpu-work-energy-efficiency-evidence.md`,
v0.4, 2026-06-07); the open work is implementation/evidence, not promotion.
Governed by FEAT-0006 and the `REQ-CPUEFF-*` rows in `docs\TRACEABILITY.md`.

- Analyzer average-watts report derived from `cpu_pkg_energy_delta_uj` /
  `cpu_power_window_ms` over distinct `cpu_power_sample_id` (average power is not
  logged; it is derived).
- Live evidence for the *enabled* RAPL path
  (`SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled`): the enabled integration path has
  not yet run in CI or on hardware, so it is not "validated".
- The `quarantine` → `validated` Evaluation gate for `cpu_pkg_energy_acquisition`
  (this is never set automatically).
- Cycle-counter / work-numerator: **resolved — no new module**
  (`docs\cpu-cycle-counter-source-decision-2026-06-07.md`). The 2026-06-07 live
  validation reported APERF/MPERF `#GP`, but that was a probe-index error: the
  shipped bin allow-lists the AMD read-only aliases `0xC00000E7`/`0xC00000E8`, so
  the work numerator (ΔAPERF) and effective frequency are reachable with the
  current bin (corrected 2026-06-09). Remaining: a corrected per-core-pinned live
  read (`tools\cpu_cycle_counter_probe.cpp`) then the cycle path
  (`docs\cpu-work-energy-live-validation-results-2026-06-07.md`).

### FEAT-0008 (watchdog hung-worker recovery) post-v1 only

FEAT-0008 v1 is implemented and verified: the `--restart` stop-timeout path now
escalates to a bounded force-terminate, automated C++/Python tests pass, and the
live deterministic suspend measurement verified the production watchdog recovery
mechanism. There is no remaining v1 recovery-path evidence to collect. The
natural load hard-freeze premise is closed on evidence as not reproducible by
load on this system; the AVX-512 escalation was rejected as the wrong instrument
for FEAT-0008's worker-specific stop-timeout trigger.

Remaining items are post-v1 options or hardening only, governed by
`docs\features\FEAT-0008-watchdog-hung-worker-recovery.md` §11:

- Make the force-kill grace period configurable if the fixed 15 s deadline proves
  wrong in practice.
- Consider applying the same escalation to plain `--stop` after timeout.
- Consider a fail-closed recovery fallback for the pre-first-write PID
  corroboration race.
- Consider canonicalizing image paths if a future install uses `subst`, mapped
  drives, junctions, or symlinked `release\` paths.

### Include ownership — decision-gated, no action yet
`docs\STRUCTURE_AND_STABILITY.md` already records converting `#include` directives
to module-qualified paths "only if the team wants stricter include ownership; the
current build keeps compatibility include roots" (`SVG_MB_CONTROL_INCLUDE_DIRS` in
`CMakeLists.txt`). This is a team decision, not a standalone next step; do nothing
until that decision is made.

### Docs archive / merge / compact pass

Partly completed 2026-06-09:

- **Done** — archived dead point-in-time snapshots under `docs\archive\`:
  `build-optimization-results.md`, `code-quality-pass-2026-05-19.md`,
  `evaluation-and-optimization-recommendations.md`.
- **Done** — merged the five overlapping Bench-vs-Control logging notes into
  `docs\bench-logging-history.md`; the archived originals remain under
  `docs\archive\` for audit history.

Still open:

- **Compact**: `modular-profile-hotswap-discussion-2026-06-06.md` (88 KB → short
  held-design pointer; FEAT-0003 is held Draft) and its `-plan` (lighter — keep the
  §5 primitive table); fold `profile-hot-swap-allow-live-decision-2026-06-06.md`
  into the decision doc's D6; consolidate the five `cpu-work-energy-*` files to ~2
  (merge the live-validation plan into the results doc; fold the resolved
  `cpu-cycle-counter-source-decision` into `cpu-work-energy-acquisition-decision`),
  preserving the live forward-gates (quarantine-exit Evaluation, remaining cycle
  path, open 400 W ceiling); reduce `discovery-control-optimization-options`,
  `discovery-dashboard-health-polling`, `discovery-next-logging-targets`, and
  `discovery-gpu-response-refinement` to closed stubs.
- **Keep as-is** (load-bearing or open work): `adaptive-cadence-design`
  (CONTROL_LOOP), `discovery-polling-logging-state` + `discovery-steady-response-control`
  (MEASUREMENT_GATE), `discovery-control-math-performance`
  (CONTROL_SIMPLIFICATION_TARGETS), `discovery-recovery-gap-audit-2026-06-04`
  (open remediations 1 & 2), `testing-harness-evaluation-2026-06-06` (open §5
  coverage gaps), and `discovery-gpu-temp-envelope` (sole record of the RTX 5090
  zero-hotspot envelope constraint — lift that fact into `CONTROL_PIPELINE_MATH.md`
  or `COOLING_STRATEGY.md` before removing it, if ever).
- Then refresh `CODE_MAP.md`'s "## Docs" taxonomy more broadly for decision
  records, feature-design notes, `cpu-temp-comparison-harness`, `next_steps`,
  and other secondary docs not yet listed. The 2026-06-09 archive/merge pass only
  updated the moved/merged file references.

Content-drift items from the same assessment were fixed 2026-06-09:

- `docs\source-aware-blend-decision-2026-05-26.md` now states that the
  channels `1`/`5` `cpu_only` claim was true for the 2026-05-26 deployment and
  later superseded by the 2026-06-09 radiator-exhaust GPU response decision.
- `docs\response-evaluation-tuning-plan.md` now distinguishes the deployed
  `release\control.json` baseline from the pending, not-yet-deployed 2026-06-09
  repo-config GPU retune.

### Optional housekeeping — low priority, defer
- Archiving closed implementation records (`docs\CONTROL_SIMPLIFICATION_TARGETS.md`,
  `docs\LOGGING_IMPROVEMENT_PLAN.md`, `docs\SCRIPT_STACK_REVIEW.md`) into a
  `docs\archive\` directory: these are already listed in `README.md` as compacted
  records kept separate from the operator workflow, and moving them churns
  `README.md` + `AGENTS.md` cross-references. Skip unless the flat layout causes
  friction.
- Splitting the 17 flat Python tests in `tests\` into mode subdirectories: the
  C++ lane is already isolated under `tests\cpp\`. A split risks the shared
  `tests\helpers.py` imports and the `python -m unittest discover tests` pattern;
  defer until the suite is large enough to justify the per-subdirectory
  `__init__.py` work.

## Prior list — corrected status

The earlier list contained seven items. Verified against the repo:

- **Add `-NoStopProcesses` to `Test-LocalCI.ps1`** — already done.
  `Test-LocalCI.ps1` already invokes `Build-Release.ps1 -NoStopProcesses
  -NoPublish` and is documented as running without stopping a live controller.
- **Promote FEAT-0006 Draft → Accepted** — stale. FEAT-0006 is already
  `Accepted` (v0.4, 2026-06-07). The real work is the downstream items above.
- **Validate cadence via `Test-LocalCI.ps1` against fresh runtime CSV** — wrong
  tool. The intent is correct (see "Require fresh runtime evidence" above), but
  `Test-LocalCI.ps1` produces no runtime CSV; use the analyze workflow / the
  comparison harness.
- **Maintain traceability** — not a discrete step; it restates a standing
  `AGENTS.md` rule (Feature Intake Gate, Change Checklist).
- **Enforce strict includes** — already captured as an optional, decision-gated
  item in `docs\STRUCTURE_AND_STABILITY.md` (see above).
- **Archive closed plans** / **organize Python tests** — optional housekeeping,
  deferred (see above).
