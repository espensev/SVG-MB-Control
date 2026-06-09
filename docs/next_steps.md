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
