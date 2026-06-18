# FEAT-0020 Actionability Plan

**Project:** svg-mb-control
**Status:** Implementation-authorized plan; live flip not authorized
**Updated:** 2026-06-18
**Owner spec:** `docs/features/FEAT-0020-standard-control-loop-power-logging.md`
**Decision:** `docs/power-logging-flip-plan-2026-06-18.md` (D-PWRLOG-1, Current)
**Build-chain review:** `docs/feat-0020-build-chain-review-2026-06-18.md`

## 1. What can be done now

FEAT-0020 is buildable for repo-local implementation and validation. The
following work can proceed without live-runtime interaction:

- add the standard control-loop GPU power sample fields and CSV rows;
- add analyzer schema v11, ingest, and report support for GPU power;
- add local tests for CSV shape, analyzer old-archive compatibility, GPU power
  summary math, and control identity;
- add a repo-owned `Set-EnergyLoggingProfile.ps1 -Enable/-Disable` workflow with
  dry-run or mocked/safe tests;
- update README/runtime/analyzer docs; and
- run `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`.

The CPU package-energy reader itself does not need worker source changes. The
implementation only needs a standard operator profile that starts the worker with
`SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled` while keeping
`SVG_MB_CONTROL_CPU_CYCLES_MODE` disabled by default.

## 2. What cannot be done without explicit live authorization

Do not do any of these as part of local implementation:

- restart the live worker;
- publish into `release\` for live use;
- run `Set-EnergyLoggingProfile.ps1 -Enable` against the live scheduled task;
- disable or re-enable the `SVG-MB Energy Safety Revert` task on the live
  machine;
- change User or Machine environment variables for the live controller; or
- mark REQ-PWRLOG-04 pass.

REQ-PWRLOG-04 closes only after a separately authorized live window that captures
a same-machine/same-build pre-flip 250 ms baseline, then a post-flip comparison
window.

## 3. Work packages

### WP0 - Documentation reconciliation

Status: complete in planning docs.

- FEAT-0020 is Accepted and implementation-authorized.
- D-PWRLOG-1 is Current.
- Traceability rows are `pending`, not `not buildable`.
- The stale `runtime_artifacts.h` implementation-plan pointer is corrected to
  `src/runtime/runtime_snapshot.h`.

### WP1 - GPU power sample and control-loop CSV

Add a per-tick, read-only board-power sample to the existing GPU thermal path.

Expected edit areas:

- `src/hardware/gpu_reader.h`
- `src/hardware/gpu_reader.cpp`
- `third_party/nvapi-controller/telemetry/src/gpu_probe.cpp`
- `src/runtime/runtime_snapshot.h`
- `src/platform/direct_runtime_snapshot.cpp`
- `src/runtime/runtime_csv_rows.cpp`
- `tests/cpp/csv_rows_tests.cpp` and/or existing Python control-loop CSV tests

Required behavior:

- `gpu_power_sample_id` advances only on a fresh successful GPU power read.
- `gpu_power_time_ms` is the GPU power read timestamp, not just the control tick.
- `gpu_power_mw` is blank when unavailable; never false zero.
- `gpu_power_source` is `nvml` only when the NVML read returns nonzero,
  otherwise `unknown`.
- `gpu_power_acquisition` is `disabled`, `unavailable`, or `nvml`.
- The new fields are emitted only by the standard control-loop CSV unless a
  later spec explicitly broadens the surface.

### WP2 - Analyzer schema v11 and report support

GPU power is a database schema/report change, not just CSV parsing.

Expected edit areas:

- `src/analyze/analyze_db.h`
- `src/analyze/analyze_db.cpp`
- `src/analyze/analyze_csv.h`
- `src/analyze/analyze_csv.cpp`
- `src/analyze/analyze_ingest_db.cpp`
- `src/analyze/analyze_report_data.*`
- `src/analyze/analyze_report_queries.cpp`
- `src/analyze/analyze_report.cpp`
- `src/analyze/analyze_report_emit.cpp`
- `tests/test_analyze_ingest.py`

Required behavior:

- bump analyzer DB schema from v10 to v11;
- migrate existing v10 DBs by adding five nullable GPU power columns;
- keep old archives without GPU power ingestible;
- summarize GPU power as instantaneous sample statistics, not CPU-style
  energy/window math;
- keep CPU package watts derived only from FEAT-0006 energy/window columns.

Recommended GPU power summary:

- mean;
- p50;
- p90;
- max;
- sample count;
- acquisition/source availability counts if practical.

### WP3 - Operator profile script

Add a reversible script pair/surface, probably:

```powershell
.\scripts\Set-EnergyLoggingProfile.ps1 -Enable
.\scripts\Set-EnergyLoggingProfile.ps1 -Disable
```

Required behavior:

- `-Enable` disables the `SVG-MB Energy Safety Revert` task, sets User
  `SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled`, keeps cycles disabled unless
  explicitly requested by a future option, and restarts only when the operator
  has authorized live interaction.
- `-Disable` re-enables the safety-revert task, sets User
  `SVG_MB_CONTROL_RAPL_ENERGY_MODE=disabled`, and restarts only under live
  authorization.
- The script must be testable or reviewable without touching the live scheduled
  task by default.

### WP4 - Contract docs

Update in the same implementation change:

- `README.md`
- `docs/RUNTIME_HOME.md`
- `docs/RUNTIME_LOGGING_AND_EVALUATION.md`
- `docs/features/FEAT-0020-standard-control-loop-power-logging.md`
- `docs/TRACEABILITY.md`

No `docs/CONTROL_PIPELINE_MATH.md` change is expected unless implementation
accidentally routes power into control computation, which FEAT-0020 forbids.

### WP5 - Validation

Before handoff:

```powershell
python -m unittest tests.test_feature_specs -v
.\scripts\Test-LocalCI.ps1 -KeepBuildDir
```

Do not use `.\build-release.ps1` or live scheduled-task operations unless the
maintainer separately authorizes release/live work.

### WP6 - Build/package chain

The existing local-CI build path is sufficient for repo-local implementation:
`.\scripts\Test-LocalCI.ps1 -KeepBuildDir`.

When implementation adds the operator script, update the release package list:

- add `scripts\Set-EnergyLoggingProfile.ps1` to `$DistExtras` in
  `scripts\Build-Release.ps1`;
- update README's release-output list for the packaged script; and
- keep this change in the same patch as the script so the release package and
  operator docs do not drift.

If implementation adds new C++ source files rather than extending existing
runtime/analyzer files, add them to `svg_mb_control_core` in `CMakeLists.txt`.
Extending existing C++ test files such as `tests/cpp/csv_rows_tests.cpp` needs no
CMake change.

## 4. Stop points

Stop after local CI and report status. Do not continue into release publishing or
live runtime verification by default.

Stop again before the live flip. The live flip needs an explicit approval
because it may restart the controller, change User env vars, and disable the
boot/logon safety-revert task.

## 5. FEAT-0021 boundary

Do not include FEAT-0021 GPU workload context fields in the FEAT-0020
implementation. FEAT-0020 is only CPU package energy plus GPU board power.
Utilization, clocks, pstate, and VRAM stay in the held FEAT-0021 draft until
FEAT-0020's GPU sample path and analyzer schema are stable.
