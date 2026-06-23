# Script Stack Review

Status: completed implementation record, compacted 2026-05-29.

This note records the outcome of the script-stack simplification work. It is no
longer an active backlog; use `README.md`, `AGENTS.md`, and `docs\CODE_MAP.md`
for the current workflow map.

## Current State

- `build-release.ps1` and `scripts\Build-Release.ps1` remain the default
  release entry points.
- `scripts\Test-LocalCI.ps1 -KeepBuildDir` is the default no-publish local
  validation path.
- Root `build.ps1` delegates to `scripts\Test-LocalCI.ps1 -KeepBuildDir`; it no
  longer owns Visual Studio setup or raw CMake orchestration.
- `scripts\Build-Release.ps1` is now a pipeline shell around focused helper
  modules: `Build.VsEnv.ps1`, `Build.Tools.ps1`, `Build.Package.ps1`,
  `Build.Info.ps1`, and `Build.Tests.ps1`.
- Python resolution is shared through `scripts\Common-Python.ps1`, used by the
  build pipeline and eval-dashboard launcher.
- Scheduled-task, watchdog, and shortcut installers share
  `Install-SVG-MB-ControlCommon.ps1`.
- Packaged scheduled-task install paths require
  `svg-mb-control-task-runner.exe`; the old VBS watchdog fallback is removed.
- `Install-SVG-MB-ControlWatchdogScheduledTask.ps1 -Run` delegates to the same
  native `--watchdog-run` path used by the registered scheduled task.
- The eval-dashboard server serves only `tools\eval_dashboard` assets plus
  explicit `/api/*` runtime endpoints.
- `tools\eval_cinebench.py` is an explicit offline helper with `--csv` and
  optional `--events`; it does not launch the controller.

## Analyzer Outcome

Native `svg-mb-control analyze` owns the analysis workflow:

- `analyze ingest` imports runtime manifests, CSV archives, events JSONL, and
  plant-model captures.
- `analyze ingest --csv <path> [--events <path>]` imports a manifest-free raw
  control-loop CSV into SQLite.
- `analyze report` emits text or JSON reports, compact decision records, and
  analysis manifests.
- Native report output includes the former Python-only gaps: GPU-envelope peak
  and threshold metrics, loop-timing and process-resource percentiles, and
  low-band-inclusive per-channel `response_boost_total_pct`.

`scripts\analyze_control_run.py` is now only a convenience wrapper for raw CSVs:
it creates a temporary DB, invokes native `analyze ingest --csv`, and forwards
native `analyze report` output. It has no independent report renderer, manifest
schema, diagnostic flags, percentile implementation, or GPU-response logic.

## Remaining Watch Points

- Build-time watchdog suspend mirrors the default task path/name from
  `Install-SVG-MB-ControlCommon.ps1` as one literal in
  `scripts\Build-Release.ps1`. A custom-installed watchdog path will be skipped
  with a verbose note.
- Runtime CSV column names still cross C++, JavaScript, and SQLite ingest. The
  runtime header producer in `src\runtime\runtime_csv_rows.cpp` is the contract;
  reader comments point back to it.
- Cross-runtime CSV parsing remains duplicated where sharing code is not
  practical (`src\analyze`, dashboard JavaScript, ad-hoc Python tools). Keep the
  CSV prologue and quoting contract documented in
  `docs\RUNTIME_LOGGING_AND_EVALUATION.md`.
- Separating release packaging from live deploy/restart is still an optional
  structural polish item in `docs\STRUCTURE_AND_STABILITY.md`.

## Verification

Validated on 2026-05-29 at commit `554cba13da64b2c0d53e6781699d28be234fb492`
with:

```powershell
.\scripts\Test-LocalCI.ps1 -KeepBuildDir
```

Result: release build succeeded, CTest passed 6/6, and the hermetic Python suite
passed 114 tests.
