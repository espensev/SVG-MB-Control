# Review-Derived Next Steps

## Status

Reviewed and rewritten 2026-06-09. These are review notes and a backlog, not work
authorization: per `AGENTS.md` (Feature Intake Gate), product-code work for a new
capability, schema/status/log field, CLI surface, or shipped-config behavior must
go through an implementation-authorized `docs\features\FEAT-*` spec first. Each
item below names the governing doc or spec.

This supersedes the earlier agent-generated list. That list was checked item by
item against the current repo; the correction is recorded under "Prior list" so
the reasoning is auditable.

## Grounded backlog

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

### Include ownership — decision-gated, no action yet
`docs\STRUCTURE_AND_STABILITY.md` already records converting `#include` directives
to module-qualified paths "only if the team wants stricter include ownership; the
current build keeps compatibility include roots" (`SVG_MB_CONTROL_INCLUDE_DIRS` in
`CMakeLists.txt`). This is a team decision, not a standalone next step; do nothing
until that decision is made.

### Docs archive / merge / compact pass — deferred until the GPU retune lands

A read-only assessment of all 63 `docs\*.md` (2026-06-09) found the historical
layer is cleanable, but the move is deferred: it churns ~20 cross-references
(`AGENTS.md`, `CODE_MAP.md`, `README.md`, `PATH_NOTES.md`, and several maintained
docs), and should not be stacked on the uncommitted GPU-curve retune
(`docs\gpu-response-curve-retune-2026-06-09.md`, `config\control.release.json`).
Run it as its own reviewable pass after that retune is committed/redeployed. Same
churn caution as the closed-records bullet below; this plan keeps those three
closed records flat.

When picked up:

- **Archive** dead point-in-time snapshots (no maintained inbound reference) into
  a new `docs\archive\`: `build-optimization-results.md`,
  `code-quality-pass-2026-05-19.md`, `evaluation-and-optimization-recommendations.md`.
- **Merge** the five overlapping Bench-vs-Control logging notes
  (`discovery-bench-cpp-priority`, `discovery-bench-logger-gap`,
  `discovery-control-bench-logging`, `discovery-current-vs-earlier`,
  `discovery-logging-parity`) into one `docs\bench-logging-history.md`, keeping the
  repo line-count census, the Bench export/no-import boundary, the two-plane
  controller-vs-evidence model, and the "don't copy the heavy stack yet" rule.
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
- Then refresh `CODE_MAP.md`'s "## Docs" taxonomy (stale — missing the decision
  records, `cpu-temp-comparison-harness`, `testing-harness-evaluation`,
  `next_steps`, and others) and the `AGENTS.md` / `README.md` inventory lists for
  any moved or merged files.

Two content-drift items from the same assessment, fixable independently of the
file moves:

- `docs\source-aware-blend-decision-2026-05-26.md` (lines 101, 167) still says
  "channels `1` and `5` remain `cpu_only`" — true as of 2026-05-26 but superseded
  by the 2026-06-09 source-aware switch; add a supersession note.
- `docs\response-evaluation-tuning-plan.md` (line 145 and the 213-218 tuning
  direction) predates the 2026-06-09 GPU curves on channels `1`/`4`/`5`; refresh
  alongside the retune writeup.

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
