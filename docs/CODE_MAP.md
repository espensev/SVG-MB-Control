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

- `src/app/app_main.{h,cpp}` — `RunApp` entry: config-path resolution and
  mode dispatch (control-loop, read-loop, write-once, evidence-log, calibrate,
  analyze, health, `--show-config`).
- `src/app/app_args.{h,cpp}` — CLI option model, value parsers, help/version
  output, and argument validation.
- `src/app/app_diagnose.{h,cpp}` — One-shot diagnostic helpers for AMD/GPU
  sampling and direct snapshot JSON.
- `src/app/app_signals.{h,cpp}` — Win32 console handler, active loop scopes,
  and cooperative stop signal state.
- `src/app/startup_banner.{h,cpp}` — Mode-specific stdout startup
  diagnostics (resolved config path, runtime home, key channel settings).

## Control Loop (`src/control/`)

- `src/control/boost_stage.{h,cpp}` — Per-stage smootherstep integrator
  shared by thermal_pressure, midband_pressure, gpu_airflow, and cpu_low_soak.
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
- `src/control/control_math.{h,cpp}` — Shared math primitives:
  smootherstep, scale, rate-limited approach — used by cadence and low-band.
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
  `RunSupervisorWorkerLoop`, tracks restart counts. FEAT-0023: while the
  worker runs it consumes the operator-written `profile.switch.request.json`
  (`TakeRuntimeProfileSwitchRequest`), validates the candidate before it
  signals a restart (only a request that resolves in the catalog and loads
  triggers `RequestRuntimeProfileCycle`), and auto-reverts to the
  last-known-good profile when a freshly-switched worker fails startup
  (`DecideAfterStartupOutcome`).
- `src/control/low_band_evidence.{h,cpp}` — Serializes the per-tick
  low-band evidence record (signal, debt, stage activation, CPU scale)
  to the event log.
- `src/control/low_band_integrator.{h,cpp}` — Advances the global
  low-band signal and debt, and per-channel staged boost, each tick.
- `src/control/machine_profile.{h,cpp}` — FEAT-0023 profile selection:
  pure `ResolveProfileSelection` precedence (explicit `--config` >
  `--profile` flag > `SVG_MB_PROFILE` env > machine id > default), plus
  catalog resolution (`ResolveProfileConfigPath` /
  `DefaultProfileCatalogDirs`) that maps a profile name to its
  `config/profiles/<name>.json` path.
- `src/control/profile_composition.{h,cpp}` — FEAT-0023 (REQ-MPROFILE-01)
  `ComposeConfigRoot`: composes a `compose`-descriptor config into one
  control-config JSON by injecting the machine-base
  (`machine_cooling_policy.v1`) per-channel `release_min_duty_pct` into the
  behavior-overlay channels; a config with no `compose` key is returned
  unchanged.
- `src/control/profile_switch_decision.{h,cpp}` — FEAT-0023 pure
  switch-decision seam: `DecideSwitchRequest` (REQ-MPROFILE-05; reject or
  activate a taken switch request after catalog and load/validate checks)
  and `DecideAfterStartupOutcome` (REQ-MPROFILE-07; keep on worker
  survival, revert to last-known-good on a startup failure), kept free of
  process spawn/signal so they are unit-testable.
- `src/control/tick_runner.{h,cpp}` — `ControlLoopRunState` and
  `RunControlTick`: per-tick sampling, channel evaluation, write
  attempts, artifact publication, and wait — the steady-state body.
- `src/control/worker_force_terminate.{h,cpp}` — FEAT-0008 bounded
  force-terminate escalation for a hung worker the `--restart` graceful
  stop cannot clear: a pure `EscalateForceTerminate` orchestration behind
  an injectable `ProcessTerminator` seam, plus the single-handle
  `Win32ProcessTerminator` adapter.

## Policy (`src/policy/`)

- `src/policy/control_policy.{h,cpp}` — Curve/blend primitives:
  `CurvePoint`, `TempBlend` enum (incl. `MaxCpuGpuSourceAware`),
  `CurveShape`, `BlendTemps`, `LookupCurve`.

## Runtime IO and State (`src/runtime/`)

- `src/runtime/csv_util.{h,cpp}` — CSV field quoting and numeric
  formatting helpers used by both the runtime logger and analyzer.
- `src/runtime/evidence_log.{h,cpp}` — Foreground `evidence-log` mode:
  AMD/GPU/fan sampling and JSONL emission with no fan writes.
- `src/runtime/evidence_signatures.{h,cpp}` — Pure change-detection /
  signature helpers split out of `evidence_log`, used to set per-field
  change flags without re-sampling.
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
- `src/runtime/runtime_csv_rows.{h,cpp}` — `RuntimeControlChannelLogState`
  and the read-loop/evidence-log row structs that define the CSV column
  contract. Repeated per-index field groups (fan snapshot, fan tach
  evidence, SIO voltage/temperature, control channel) are emitted from
  shared `CsvColumn<Entity>` descriptor tables so the header and row
  builders iterate one source of truth and cannot drift.
- `src/runtime/runtime_event_log.{h,cpp}` — `RuntimeLogEvent` struct,
  `AppendRuntimeEvent` JSONL writer, and the event-count probe.
- `src/runtime/runtime_health.{h,cpp}` — `EvaluateRuntimeHealth` and
  `AssessHealthState`: turns `control_runtime.json` + process state
  into `healthy / degraded / stale / stopped / failed`.
- `src/runtime/runtime_lifecycle.{h,cpp}` — Stop-request and
  reset-request file management (`stop.request.json`,
  `circuit_breaker_reset.request.json`).
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
- `src/runtime/runtime_write_policy.{h,cpp}` — Loads
  `runtime_policy_write_live.json` and answers "is this channel blocked / are
  writes enabled?".
- `src/runtime/write_orchestrator.{h,cpp}` — Write-once mode: baseline capture
  from a runtime snapshot, freshness check, sidecar upsert, direct duty write,
  hold, restore. Also owns `ReconcilePendingWrites` for orphaned-sidecar
  recovery.

## Hardware Backends (`src/hardware/`)

- `src/hardware/amd_reader.{h,cpp}` — AMD SMN CPU telemetry reader
  (Tctl/Tdie) backed by the PawnIO driver.
- `src/hardware/amd_decode.h` — AMD SMN decode math (Tctl/Tdie and
  per-CCD Tdie with Zen2/Zen4 CCD-base layout selection), unit-tested by
  `amd_decode_tests.cpp`.
- `src/hardware/cpu_cycles.h` — Pure math for AMD APERF/MPERF cycle evidence.
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
- `src/hardware/rapl_energy.h` — Pure math for AMD RAPL package-energy evidence.
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
- `src/platform/machine_identity.{h,cpp}` — FEAT-0023 machine-identity
  resolution (`ResolveMachineId` / pure `ResolveMachineIdFrom`): a
  non-empty trimmed `machine_id.txt` override wins, otherwise the host name
  (`GetComputerNameW`) is trimmed and lower-cased, used to auto-select a
  catalog profile when no `--config` / `--profile` is given.
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
- `tests/cpp/analyze_report_tests.cpp` — CTest coverage for native
  analyze-report pure helpers (percentiles, band summaries,
  diagnostic flags).
- `tests/cpp/boost_stage_tests.cpp` — CTest trajectory-equivalence
  coverage for the table-driven boost-stage updater.
- `tests/cpp/control_loop_config_tests.cpp` — CTest coverage for
  control-loop config parsing and validation round trips.
- `tests/cpp/control_math_tests.cpp` — CTest coverage for shared
  smootherstep, scaling, and rate-limit math primitives.
- `tests/cpp/csv_rows_tests.cpp` — CTest coverage for runtime CSV
  header/row alignment and representative present/absent indexed
  fields across read-loop, evidence-log, and control-loop rows.
- `tests/cpp/pawnio_binary_tests.cpp` — CTest C++ coverage for PawnIO
  binary resolution, load, and SHA-256 verification.
- `tests/cpp/channel_write_tests.cpp` — CTest coverage for the
  per-channel write-evaluation path.
- `tests/cpp/amd_decode_tests.cpp` — CTest coverage for the AMD SMN
  decode math in `amd_decode.h`.
- `tests/cpp/cpu_cycles_tests.cpp` — CTest coverage for the AMD cycle
  math in `cpu_cycles.h`.
- `tests/cpp/rapl_energy_tests.cpp` — CTest coverage for the AMD RAPL
  package-energy math in `rapl_energy.h`.
- `tests/cpp/worker_force_terminate_tests.cpp` — CTest coverage for the
  FEAT-0008 force-terminate escalation orchestration (worker-first
  ordering, image guard, PID corroboration, single-shot bound) via a fake
  `ProcessTerminator`.
- `tests/test_analyze_ingest.py` — End-to-end `analyze ingest`,
  `analyze prune`, and native `analyze report` coverage.
- `tests/test_analyzer.py` — Integration tests for the Python
  `analyze_control_run.py` wrapper around native `analyze ingest --csv` plus
  `analyze report`.
- `tests/test_calibration.py` — Calibration sequence parsing and
  step-wise execution coverage.
- `tests/test_config_contracts.py` — `control.json` schema validation
  and required-field coverage.
- `tests/test_feature_specs.py` — Feature-spec registry, traceability,
  promotion-gate, and verification-log consistency checks.
- `tests/test_machine_cooling_policy.py` — Coverage for the
  `config/machines/snd-desk.cooling.policy.json` cooling-policy contract.
- `tests/test_control_loop.py` — `control-loop` mode end-to-end
  hermetic coverage.
- `tests/test_cpu_temp_power.py` — Coverage for the power-normalized CPU
  temperature evaluator script, including package-watt banding and
  GPU-confound splitting.
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
- `tests/test_watchdog_force_terminate.py` — FEAT-0008 integration
  coverage: a suspended (hung) worker is force-terminated by `--restart`
  and relaunched with a new PID, the orphaned baseline is reconciled, and
  a graceful worker is never force-terminated.

## Scripts

- `scripts/Build-Release.ps1` — Canonical release entry point:
  CMake configure, build, CTest, hermetic Python suite, publish to
  `release\`, archive, and watchdog/controller restart.
- `scripts/Start-EvalDashboard.ps1` — Launches the local eval
  dashboard Python server.
- `scripts/Test-LocalCI.ps1` — CI-style local validation: release
  build + tests without publishing or restarting the live controller.
- `scripts/analyze_control_run.py` — Thin wrapper that ingests a raw
  control-loop CSV into a temporary DB via the in-repo svg-mb-control.exe
  and forwards native `analyze report` output (no analysis of its own).
- `scripts/Build.VsEnv.ps1`, `scripts/Build.Tools.ps1`,
  `scripts/Build.Package.ps1`, `scripts/Build.Info.ps1`,
  `scripts/Build.Tests.ps1` — dot-sourced helper modules of
  `Build-Release.ps1` (VS-env bootstrap, tool resolution, dist packaging,
  version/build-info and archive helpers, and the CTest/hermetic test lanes).
- `scripts/Common-Python.ps1` — shared Python interpreter resolver,
  dot-sourced by `Build-Release.ps1` and `Start-EvalDashboard.ps1`.
- `scripts/Compare-CpuTemps.ps1` — read-only harness that bins control-loop
  CSV `cpu_tctl_c` by sustained `system_cpu_busy_pct` into idle/low/high and
  records a per-setting comparison ledger.
- `scripts/analyze_cpu_temp_power.py` — read-only evaluator that bins distinct
  FEAT-0006 CPU package-energy windows by package watts, reports apparent
  `theta C/W`, separates GPU-confounded windows, surfaces per-CCD Tdie
  (`ccd1_tdie_c`/`ccd2_tdie_c`/`ccd_delta_c`, parsed from `amd_sensor_summary`
  via `control_csv.parse_ccd_temps`), and reads radiator/context channel
  membership from the machine cooling policy.
- `scripts/cpu_config_fingerprint.py` — read-only analyzer (skeleton) that builds
  a per-run config-pure fingerprint (idle package-power floor, per-busy-band
  watts, effective MHz/W, CCD balance) and segments runs into auto-labeled
  regimes (median/MAD step detection over exact `git_hash`/`config_sha256` cuts)
  to detect BIOS/Curve-Optimizer/PBO changes from telemetry without operator
  annotation. Emits `svg_mb_control.cpu_fingerprint.v1`.
- `scripts/control_csv.py` — shared read-only helper (one source of truth):
  splits a `svg_mb_control.log.v1` CSV into (meta, header, rows), reads columns
  by name, and parses per-CCD Tdie (`parse_ccd_temps`). Consumed by
  `analyze_power_lead.py`, `score_energy_session.py`, `analyze_cpu_temp_power.py`,
  and `cpu_config_fingerprint.py`.
- `scripts/Install-CpuTempBaselineTask.ps1` — registers/removes a scheduled
  task that runs `Compare-CpuTemps.ps1` on a cadence for a long-term baseline.

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
- `config/overlays/release.behavior.json` — FEAT-0023 behavior overlay:
  the tunable `control_loop` curves / boosts / cadence with each
  channel's `min_duty_pct` removed (the physical floor comes from the
  machine base at composition time).
- `config/profiles/snd-desk-composed.json` — FEAT-0023 composition
  descriptor: a `compose` block (`machine_base`
  `../machines/snd-desk.cooling.policy.json` + `behavior_overlay`
  `../overlays/release.behavior.json`) that `ComposeConfigRoot` merges,
  selectable with `--profile snd-desk-composed`.

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

Feature intake, traceability, and verification:

- `docs/features/README.md` — Feature registry, current priority queue,
  promotion rules, and decisions-owed index.
- `docs/features/FEAT-*.md` — Per-feature specs; these are the implementation
  contracts for feature work once accepted and authorized.
- `docs/TRACEABILITY.md` — Requirement-to-verification map.
- `docs/FEATURE_VERIFICATION_CHECKLIST.md` — Checklist used while implementing
  or verifying features.

Current contract and reference docs (kept aligned with code and shipped
behavior):

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
- `docs/SERVICE_PROBE.md` — Service-feasibility probe contract.
- `docs/MEASUREMENT_GATE.md` — Sensor validity gating rules.
- `docs/NORMAL_RUNTIME_AIRFLOW_PROFILE.md` — Steady-state airflow
  baseline (maintained).
- `docs/response-evaluation-tuning-plan.md` — Response evaluation
  plan (maintained).

Maintained evaluation and operational records:

- `docs/logging-next-targets-2026-06-18.md` — Current logging-target
  decision record across FEAT-0020/0021/0022.
- `docs/runtime-logging-health-decision-2026-06-20.md` — FEAT-0022
  logging-health decision.
- `docs/power-temp-comparison-snapshot-2026-06-18.md` — Preserved
  standard-loop CPU/GPU watts beside temperatures for future comparisons.
- `docs/cpu-temp-comparison-harness.md` — CPU temperature comparison
  harness workflow, current busy-band trend snapshot, and power-normalized
  companion method.
- `docs/next_steps.md` — Maintained topical backlog; not implementation
  authorization by itself.

Decision and design records that remain useful for navigation:

- `docs/source-aware-blend-decision-2026-05-26.md` — Current
  source-aware blend decision and verification record.
- `docs/control-latency-reduction-design-2026-06-18.md` — FEAT-0017,
  FEAT-0018, and FEAT-0019 latency-audit direction.
- `docs/profile-hot-swap-decision-2026-06-03.md` — FEAT-0003
  design-capture decision record; not scheduled work.
- `docs/hwaccess-health-signal-decision-2026-06-18.md` — FEAT-0004
  hardware-access health-signal direction.
- `docs/actuation-confirmation-decision-2026-06-18.md` — FEAT-0005
  write-actuation confirmation direction.
- `docs/analyze-db-run-purge-decision-2026-06-18.md` — FEAT-0016
  analyzer DB retention/prune direction.

Compacted / archived records (closed topics; keep these short and
do not merge them into operator docs unless the topic is reopened):

- `docs/archive/implemented-plans/CONTROL_SIMPLIFICATION_TARGETS.md` — Completed simplification
  record for the closed 2026-05-26 target list.
- `docs/archive/implemented-plans/LOGGING_IMPROVEMENT_PLAN.md` — Completed logging/analyzer
  implementation record.
- `docs/archive/implemented-plans/SCRIPT_STACK_REVIEW.md` — Completed script-stack
  simplification record.
- `docs/archive/implemented-plans/testing-and-hotpath-simplification-review-2026-06-10.md`
  — Applied testing/script-stack and runtime hot-path simplification review.
- `docs/bench-logging-history.md` — Consolidated history of the older
  Bench-vs-Control logging discovery notes.
- `docs/archive/implemented-plans/` — Completed implementation plans and
  executed procedures kept for audit history, separated from active plans.
- `docs/archive/modular-profile-hotswap-discussion-2026-06-06.md` — Compacted
  FEAT-0003 discussion pointer.
- `docs/archive/modular-profile-hotswap-plan-2026-06-06.md` — Compacted FEAT-0003
  primitive-table pointer.
- `docs/archive/profile-hot-swap-allow-live-decision-2026-06-06.md` — Compacted
  support record; the decision lives in
  `docs/profile-hot-swap-decision-2026-06-03.md`.
- `docs/archive/` — Historical discovery, parked planning, and evidence records
  that are useful for audit trails but should not be treated as current
  navigation or implementation queues.

Historical / discovery (per `AGENTS.md`, treat as context, not
current contract, unless re-validated). Closed redirect stubs for control
optimization, dashboard health polling, GPU response refinement, and next
logging targets were folded into the current contract docs/backlog on
2026-06-20 and deleted to reduce Markdown sprawl:

- `docs/adaptive-cadence-design-2026-05-19.md`
- `docs/discovery-gpu-temp-envelope.md`
- `docs/archive/spec-and-backlog-structure-assessment-2026-06-18.md`
- `docs/discovery-polling-logging-state.md`
- `docs/discovery-recovery-gap-audit-2026-06-04.md`
- `docs/discovery-steady-response-control.md`
- `docs/testing-harness-evaluation-2026-06-06.md`
- `docs/archive/build-optimization-results.md`
- `docs/archive/code-quality-pass-2026-05-19.md`
- `docs/archive/evaluation-and-optimization-recommendations.md`
- `docs/archive/discovery-bench-cpp-priority.md`
- `docs/archive/discovery-bench-logger-gap.md`
- `docs/archive/discovery-control-bench-logging.md`
- `docs/archive/discovery-current-vs-earlier.md`
- `docs/archive/discovery-logging-parity.md`
