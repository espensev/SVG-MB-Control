# Code Map

Per-file responsibility map for `SVG-MB-Control`. Use this as a navigation
aid alongside `docs/STRUCTURE_AND_STABILITY.md` (which describes module
boundaries) and `AGENTS.md` (the canonical agent contract).

Files are paired as `name.{h,cpp}` when the header and implementation share
the same responsibility; otherwise they are listed separately.

## Executable Entry

- `src/main.cpp` — `wmain` shim that forwards `argc/argv` to
  `svg_mb_control::RunApp`.
- `src/pch.{h,cpp}` — Precompiled header pulling in Windows, STL, and
  vendored `nlohmann/json` headers.

## App Dispatch (`src/app/`)

- `src/app/app_main.{h,cpp}` — `RunApp` entry: CLI parsing, config-path
  resolution, mode dispatch (control-loop, read-loop, write-once,
  evidence-log, calibrate, analyze, health, `--show-config`), and console
  control handling.
- `src/app/startup_banner.{h,cpp}` — Mode-specific stdout startup
  diagnostics (resolved config path, runtime home, key channel settings).

## Control Loop (`src/control/`)

- `src/control/cadence_score.{h,cpp}` — Per-tick slew score and the
  upward-only cadence integrator that produces the effective tick
  interval.
- `src/control/calibration.{h,cpp}` — Calibration sequence parser and
  step-wise runner that drives discrete duty/temperature sweeps.
- `src/control/channel_evaluator.{h,cpp}` — Per-channel evaluation:
  temperature blend (including `max_cpu_gpu_source_aware` with the
  CPU-hot guard), curve lookup, demand smoothing, pressure boosts, and
  sensor-event detection.
- `src/control/channel_write.{h,cpp}` — Channel-write decision helpers:
  baseline capture, sensor-event emission, hold-restore expiration,
  write-failure / post-write side effects, and `TryApplyChannelSetpoint`.
- `src/control/control_config.{h,cpp}` — `ControlConfig` struct and
  top-level config-file loader (poll-ms, policy path, calibration
  block).
- `src/control/control_config_print.{h,cpp}` — Operator-facing
  `--show-config` summary in human and JSON formats.
- `src/control/control_loop.{h,cpp}` — `ControlLoop` lifecycle wrapper:
  startup/shutdown choreography, public surface, and dispatch into
  `tick_runner`. Steady-state per-tick body lives elsewhere.
- `src/control/control_loop_config.cpp` — Control-loop subtree and
  per-channel JSON parsing/validation (`LoadLowBandConfig`,
  `LoadChannelConfig`). Public declaration stays in `control_loop.h`.
- `src/control/control_runtime_context.{h,cpp}` — `ChannelState` and
  `ControlRuntimeContext` structs: per-channel mutable runtime state
  (duty, smoothed demand, boost terms, hold deadlines, primary-temp
  source) plus shared context handles.
- `src/control/control_scheduler.{h,cpp}` — Tick scheduling: timer
  resolution scope, process resource sampling, sleep-until math.
- `src/control/control_status_writer.{h,cpp}` — Builds the per-tick
  `RuntimeControlChannelLogState` vector from `ChannelState` for CSV
  and status emission (no JSON write — that lives in `runtime_status`).
- `src/control/control_supervisor.{h,cpp}` — Supervised long-running
  mode: spawns/restarts the worker process, owns
  `RunSupervisorWorkerLoop`, tracks restart counts.
- `src/control/low_band_evidence.{h,cpp}` — Serializes the per-tick
  low-band evidence record (signal, debt, stage activation, CPU scale)
  to the event log.
- `src/control/low_band_integrator.{h,cpp}` — Advances the global
  low-band signal and debt, and per-channel staged boost, each tick.
- `src/control/tick_runner.{h,cpp}` — `ControlLoopRunState` and
  `RunControlTick`: per-tick sampling, channel evaluation, write
  attempts, artifact publication, and wait — the steady-state body.

## Policy (`src/policy/`)

- `src/policy/control_policy.{h,cpp}` — Curve/blend primitives:
  `CurvePoint`, `TempBlend` enum (incl. `MaxCpuGpuSourceAware`),
  `CurveShape`, `BlendTemps`, `LookupCurve`.
- `src/policy/runtime_write_policy.{h,cpp}` — Loads
  `runtime_policy_write_live.json` and answers "is this channel
  blocked / are writes enabled?".
- `src/policy/write_orchestrator.{h,cpp}` — Write-once mode:
  baseline capture from a runtime snapshot, freshness check, sidecar
  upsert, direct duty write, hold, restore. Also owns
  `ReconcilePendingWrites` for orphaned-sidecar recovery.

## Runtime IO and State (`src/runtime/`)

- `src/runtime/csv_util.{h,cpp}` — CSV field quoting and numeric
  formatting helpers used by both the runtime logger and analyzer.
- `src/runtime/evidence_log.{h,cpp}` — Foreground `evidence-log` mode:
  AMD/GPU/fan sampling and JSONL emission with no fan writes.
- `src/runtime/gpu_evidence_csv.{h,cpp}` — GPU-evidence CSV header
  builder and per-sample row appender used by `evidence-log`.
- `src/runtime/json_io.{h,cpp}` — Defensive JSON accessors
  (`JsonStringOr`, `JsonBoolOr`, …), atomic file writes, and the
  schema-versioned object constructor.
- `src/runtime/pending_writes.{h,cpp}` — Read/write/upsert/remove
  for the `pending_writes.json` sidecar.
- `src/runtime/read_loop.{h,cpp}` — Long-running read-only supervisor:
  samples AMD/GPU/fans and republishes `current_state.json` and the
  read-loop runtime-status schema.
- `src/runtime/runtime_artifacts.h` — Umbrella header that re-exports
  `runtime_paths`, `runtime_csv_archive`, and `runtime_event_log` for
  legacy includers.
- `src/runtime/runtime_csv_archive.{h,cpp}` — `RuntimeCsvLogger`:
  rotating CSV writer with manifest tracking and archive directory
  layout.
- `src/runtime/runtime_csv_rows.{h,cpp}` — `RuntimeControlRow`,
  `RuntimeControlChannelLogState`, and read-loop row structs that
  define the CSV column contract.
- `src/runtime/runtime_event_log.{h,cpp}` — `RuntimeLogEvent` struct,
  `AppendRuntimeEvent` JSONL writer, and the event-count probe.
- `src/runtime/runtime_health.{h,cpp}` — `EvaluateRuntimeHealth` and
  `AssessHealthState`: turns `control_runtime.json` + process state
  into `healthy / degraded / stale / stopped / failed`.
- `src/runtime/runtime_lifecycle.{h,cpp}` — Stop-request and
  reset-request file management (`runtime/stop`, `runtime/reset`).
- `src/runtime/runtime_paths.{h,cpp}` — `RuntimeArtifactNaming`, the
  ISO-8601 local formatter, and resolvers for log/manifest/event
  paths.
- `src/runtime/runtime_snapshot.{h,cpp}` — `RuntimeSnapshot` struct
  holding the per-tick AMD/GPU/fan reading bundle.
- `src/runtime/runtime_status.{h,cpp}` — Single owner of
  `control_runtime.json` across both schemas: typed
  `RuntimeStatusSnapshot` reader plus `WriteControlLoopStatus` (v4)
  and `WriteReadLoopStatus` (v1) writers.
- `src/runtime/runtime_supervisor_state.{h,cpp}` — Supervisor sidecar
  with restart counts, worker exit codes, and last-restart timestamps.

## Hardware Backends (`src/hardware/`)

- `src/hardware/amd_reader.{h,cpp}` — AMD SMN CPU telemetry reader
  (Tctl/Tdie) backed by the PawnIO driver.
- `src/hardware/fan_writer.{h,cpp}` — `FanWriter` abstraction, result
  codes, and strategy selection between SIO and simulation.
- `src/hardware/fan_writer_internal.h` — Result-factory helpers and
  the internal strategy interface shared by SIO and simulated writers.
- `src/hardware/gpu_reader.{h,cpp}` — NVIDIA telemetry reader: per-fan
  RPM/duty, hotspot/edge/memory temperatures, the GPU envelope, power
  draw, and boost terms via NVAPI.
- `src/hardware/pawnio_binary.{h,cpp}` — PawnIO module-binary
  resolver, loader, and SHA-256 verifier; provenance is in
  `third_party/pawnio/README.md`.
- `src/hardware/simulated_fan_writer.cpp` — Simulation strategy used
  under `SVG_MB_CONTROL_SIM_DIRECT_WRITE_MODE` for hermetic tests.
- `src/hardware/sio_fan_writer.cpp` — Production strategy backed by
  vendored `SVG-MB-SIO`.

## Platform Shims (`src/platform/`)

- `src/platform/direct_runtime_snapshot.{h,cpp}` — Composes
  `AmdReader` + `GpuReader` + `FanWriter` into a single
  `RuntimeSnapshot` producer for direct sampling paths.
- `src/platform/env_util.{h,cpp}` — `GetEnvWithDefault` helper for
  reading env vars with a fallback.
- `src/platform/file_hash.{h,cpp}` — Shared streaming SHA-256 helper for
  runtime and analyzer artifact identity.
- `src/platform/runtime_singleton.{h,cpp}` — Cross-process named
  mutex that enforces single-controller-per-runtime-home.
- `src/platform/runtime_util.{h,cpp}` — `ProcessIsActive` and
  ISO-8601 parse/format helpers shared across modules.
- `src/platform/service_probe.{h,cpp}` — Read-only feasibility probe
  for Session 0 / LocalSystem service migration; see
  `docs/SERVICE_PROBE.md`.
- `src/platform/task_runner.cpp` — Separate hidden `wWinMain` binary
  for scheduled tasks: resolves `svg-mb-control.exe`, launches it under
  `NUL` stdio, forwards start/stop/restart/status commands, and runs the
  watchdog health/restart check.
- `src/platform/windows_lean.h` — Central `WIN32_LEAN_AND_MEAN` /
  `NOMINMAX` guard before `<windows.h>` includes.

## Analyzer (`src/analyze/`)

- `src/analyze/analyze_cli.{h,cpp}` — `analyze` subcommand dispatcher
  (`ingest`, `prune`, `report`) with shared option parsing.
- `src/analyze/analyze_channel_sample_columns.{h,cpp}` —
  `tick_channel_samples` descriptor: analyzer channel sample column names,
  SQLite types, generated DDL/insert/select SQL, CSV field names, select
  indexes, and the `ParsedChannelSample` binder.
- `src/analyze/analyze_csv.{h,cpp}` — CSV row parser that
  reconstructs per-tick telemetry samples from archived runtime CSVs.
- `src/analyze/analyze_db.{h,cpp}` — SQLite connection wrapper,
  statement RAII, and schema migration helpers.
- `src/analyze/analyze_ingest.{h,cpp}` — Ingest orchestrator:
  archive walk, manifest pairing, sample parse, summary emission.
- `src/analyze/analyze_ingest_db.{h,cpp}` — DB write operations,
  split into `IngestManifests`, `IngestEvents`, and
  `IngestPlantModels` with idempotency checks.
- `src/analyze/analyze_json_artifacts.{h,cpp}` — Manifest and event
  JSON parsers used by ingest.
- `src/analyze/analyze_prune.{h,cpp}` — Offline retention pruner for
  archive bundles (age- and count-based).
- `src/analyze/analyze_report.{h,cpp}` — Public report options and
  orchestration entry point for native `analyze report`.
- `src/analyze/analyze_report_data.{h,cpp}` — Shared report data model and
  percentile/band summarization helpers.
- `src/analyze/analyze_report_queries.{h,cpp}` — Report DB reads, tick
  banding, channel/event aggregation, manifest evidence loading, and response
  delay detection.
- `src/analyze/analyze_report_emit.{h,cpp}` — Text/JSON report emitters,
  analysis manifest writing, and compact decision records.

## Tests

- `tests/__init__.py` — Python test package marker.
- `tests/helpers.py` — Shared Python helpers: subprocess driver, temp
  runtime-home factory, CSV/JSON readers, simulation env builder.
- `tests/cpp/core_smoke_tests.cpp` — CTest C++ coverage for
  `control_policy`, CSV utilities, and core helpers.
- `tests/cpp/pawnio_binary_tests.cpp` — CTest C++ coverage for PawnIO
  binary resolution, load, and SHA-256 verification.
- `tests/test_analyze_ingest.py` — End-to-end `analyze ingest`,
  `analyze prune`, and native `analyze report` coverage.
- `tests/test_analyzer.py` — Tests for the Python
  `analyze_control_run.py` offline analyzer.
- `tests/test_calibration.py` — Calibration sequence parsing and
  step-wise execution coverage.
- `tests/test_config_contracts.py` — `control.json` schema validation
  and required-field coverage.
- `tests/test_control_loop.py` — `control-loop` mode end-to-end
  hermetic coverage.
- `tests/test_eval_dashboard.py` — `tools/eval_dashboard` HTTP API
  coverage.
- `tests/test_evidence_log.py` — `evidence-log` mode coverage.
- `tests/test_read_loop.py` — `read-loop` mode coverage including
  publish/restart paths.
- `tests/test_runtime_health.py` — Health detection coverage across
  healthy/degraded/stale/stopped/failed states.
- `tests/test_runtime_singleton.py` — Cross-process named-mutex
  enforcement coverage.
- `tests/test_service_probe.py` — Service-feasibility probe coverage.
- `tests/test_smoke.py` — Top-level CLI smoke and staged control-app
  smoke helper.
- `tests/test_write_once.py` — `write-once` mode coverage including
  sidecar upsert and restore.

## Scripts

- `scripts/Build-Release.ps1` — Canonical release entry point:
  CMake configure, build, CTest, hermetic Python suite, publish to
  `release\`, archive, and watchdog/controller restart.
- `scripts/Start-EvalDashboard.ps1` — Launches the local eval
  dashboard Python server.
- `scripts/Test-LocalCI.ps1` — CI-style local validation: release
  build + tests without publishing or restarting the live controller.
- `scripts/analyze_control_run.py` — Legacy direct-CSV compatibility
  analyzer for captures that have not been ingested into the native
  analysis DB.

## Top-Level PowerShell

- `build.ps1` — Convenience delegate to `scripts/Test-LocalCI.ps1
  -KeepBuildDir`.
- `build-release.ps1` — Thin delegate to `scripts/Build-Release.ps1`.
- `Install-SVG-MB-ControlCommon.ps1` — Shared helper functions for the
  scheduled-task, watchdog, and shortcut installers.
- `Install-SVG-MB-ControlScheduledTask.ps1` — Install/start/stop/
  status/remove for the main controller scheduled task.
- `Install-SVG-MB-ControlShortcut.ps1` — Start-menu / desktop
  shortcut installer.
- `Install-SVG-MB-ControlWatchdogScheduledTask.ps1` — Watchdog
  scheduled task installer (uses `task_runner.cpp`'s hidden binary).

## Config

- `config/control.example.json` — Reference control configuration:
  poll-ms, policy path, per-channel curves, blends, guards, low-band
  parameters.
- `config/control.release.json` — Production control configuration
  shipped with `release\`.
- `config/runtime_policy_write_live.json` — Write-policy contract
  (writes enabled, restore-on-exit, blocked channels).
- `config/machines/snd-desk.cooling.policy.json` — Machine-specific
  fan topology, pressure-balance strategy, and cooling-policy data
  behind the shipped profile.

## Tools

- `tools/eval_cinebench.py` — Offline CSV analyzer that summarizes a
  Cinebench-style load window (pre/load/post Tctl stats, cooldown
  times, and load-window event counts) from an archived control-loop
  CSV plus an optional events JSONL. Takes `--csv` and `--events`; it
  launches nothing and computes stats inline.
- `tools/eval_dashboard/` — Local web dashboard for offline/live run
  inspection. `index.html` and `styles.css` carry the layout;
  `dashboard.js` parses CSV/event JSONL client-side; `selftest.js`
  is the browser-side test harness; `server.py` exposes tail and
  status HTTP endpoints.

## Docs

Current reference docs (the `AGENTS.md` navigation list plus other
docs that are kept current):

- `docs/STRUCTURE_AND_STABILITY.md` — Module boundaries and migration
  status.
- `docs/CONTROL_LOOP.md` — Control-loop runtime behavior.
- `docs/READ_LOOP.md` — Read-loop runtime behavior.
- `docs/WRITE_ORCHESTRATION.md` — Write-once and runtime write policy.
- `docs/CONTROL_PIPELINE_MATH.md` — Maintained numerical reference
  for the control identity.
- `docs/COOLING_STRATEGY.md` — SND-DESK fan topology, pressure strategy,
  floor philosophy, and fan relationship rules.
- `docs/RUNTIME_HOME.md` — Runtime sidecar, status, log, manifest,
  and archive layout.
- `docs/RUNTIME_LOGGING_AND_EVALUATION.md` — Tuning, runtime
  evidence, analyzer workflow, logging gaps.
- `docs/CONTROL_SIMPLIFICATION_TARGETS.md` — Tracked simplification
  candidates.
- `docs/SERVICE_PROBE.md` — Service-feasibility probe contract.
- `docs/MEASUREMENT_GATE.md` — Sensor validity gating rules.
- `docs/NORMAL_RUNTIME_AIRFLOW_PROFILE.md` — Steady-state airflow
  baseline (maintained).
- `docs/response-evaluation-tuning-plan.md` — Response evaluation
  plan (maintained).
- `docs/source-aware-blend-decision-2026-05-26.md` — Current source-
  aware blend decision and verification record.

Historical / discovery (per `AGENTS.md`, treat as context, not
current contract, unless re-validated):

- `docs/adaptive-cadence-design-2026-05-19.md`
- `docs/build-optimization-results.md`
- `docs/code-quality-pass-2026-05-19.md`
- `docs/evaluation-and-optimization-recommendations.md`
- `docs/LOGGING_IMPROVEMENT_PLAN.md`
- `docs/discovery-bench-cpp-priority.md`
- `docs/discovery-bench-logger-gap.md`
- `docs/discovery-control-bench-logging.md`
- `docs/discovery-control-optimization-options.md`
- `docs/discovery-current-vs-earlier.md`
- `docs/discovery-dashboard-health-polling.md`
- `docs/discovery-gpu-response-refinement.md`
- `docs/discovery-gpu-temp-envelope.md`
- `docs/discovery-logging-parity.md`
- `docs/discovery-next-logging-targets.md`
- `docs/discovery-polling-logging-state.md`
- `docs/discovery-steady-response-control.md`
