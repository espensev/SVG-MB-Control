# Structure and Stability Plan

## Current Structure

`SVG-MB-Control` should stay as one standalone runtime repo. The long-term
stability improvement is not another repo split; it is the clearer internal
module layout and testable core library now used by the build.

Current layout:

```text
src/
  app/        thin executable entry, CLI parsing, mode dispatch
  control/    loop orchestration, channel evaluator, boost stages,
              cadence, control math, status model
  runtime/    runtime store, CSV logger, event log, JSON IO, runtime
              write policy, read-loop + write-once mode entry points
  hardware/   AMD, GPU, SIO fan backends
  platform/   Windows timer, process metrics, HANDLE wrappers
  policy/     curve lookup, temperature blending, curve shape
  analyze/    run ingestion, pruning, reporting
```

`svg_mb_control_core` is the static library target for non-app implementation
code. The `svg-mb-control.exe` target contains the executable entry point,
app-mode dispatch, startup banners, resources, and links that core library.

## Why

The current repo has the right ownership boundary: one executable owns telemetry,
policy, runtime state, writes, restore, and logs. The structural risk used to
be that implementation responsibilities were concentrated in large translation
units, especially the control loop. That has been addressed: `control_loop.cpp`
is now a thin lifecycle wrapper, and the steady-state per-tick body, cadence
score, low-band integrator, and channel-write decisions live in their own
modules (see `control/` below).

A core library gives three stability benefits:

- control logic can be unit-tested without launching the executable,
- runtime IO and hardware IO can be mocked or replaced behind small interfaces,
- future refactors can move files without changing the product boundary.

## Module Responsibilities

`app/`

- thin `wmain` wrapper (`main.cpp`),
- mode dispatch orchestrator (`app_main.cpp`: `RunApp`),
- command-line parsing (`app_args.{h,cpp}`: `CliOptions`,
  `ParseCliOptions`, `PrintUsage`, `PrintVersion`),
- Win32 console signal handler and RAII scopes
  (`app_signals.{h,cpp}`: `ConsoleCtrlScope`, `ActiveControlLoopScope`,
  `ActiveReadLoopScope`, `StopSignaled()`),
- one-shot diagnostic modes
  (`app_diagnose.{h,cpp}`: `RunDiagnoseAmd`, `RunDiagnoseGpu`,
  `SampleDirectSnapshotJson`),
- process startup banner output (`startup_banner.cpp`).

`control/`

- control-loop lifecycle (`control_loop.cpp`: startup, shutdown, dispatch),
- per-tick body (`tick_runner.cpp`: sampling, channel decisions, artifacts, wait),
- channel evaluation (`channel_evaluator.cpp`: curve, smoothing, boost composition, rate limit),
- boost overlays (`boost_stage.cpp`: per-stage smootherstep integrator shared by thermal_pressure, midband_pressure, gpu_airflow, cpu_low_soak),
- channel-write gates (`channel_write.cpp`: deadband, cooldown, breaker, hold restore),
- adaptive cadence (`cadence_score.cpp`: slew score, effective tick interval),
- tick scheduling, timer resolution, and process resource sampling (`control_scheduler.cpp`),
- shared math primitives (`control_math.{h,cpp}`: smootherstep, scale, rate-limited approach — used by cadence + low-band),
- low-band integrator (`low_band_integrator.cpp`: signal, debt, stage activation),
- low-band evidence serialization (`low_band_evidence.cpp`),
- control status publication shape (`control_status_writer.cpp`),
- supervisor and run-mode dispatch (`control_supervisor.cpp`),
- bounded hung-worker force-terminate escalation
  (`worker_force_terminate.cpp`),
- config loading, validation, and CLI formatting (`control_loop_config.cpp`, `control_config.cpp`, `control_config_print.cpp`),
- active config-path selection by precedence (`machine_profile.{h,cpp}`: `ResolveProfileSelection`, explicit `--config` > `--profile` > `SVG_MB_PROFILE` > machine id > default),
- machine-base + behavior-overlay composition (`profile_composition.{h,cpp}`: `ComposeConfigRoot`),
- supervisor switch-request and post-startup-outcome decisions (`profile_switch_decision.{h,cpp}`: `DecideSwitchRequest`, `DecideAfterStartupOutcome`),
- mutable runtime state and shared context (`control_runtime_context.cpp`),
- calibration sequence parsing and execution (`calibration.cpp`).

`runtime/`

- runtime-home path handling,
- atomic JSON writes,
- CSV logger,
- event JSONL writer,
- pending-write sidecars,
- run summary/manifest plumbing,
- runtime write authorization policy (`runtime_write_policy.{h,cpp}`),
- runtime request-file lifecycle (`runtime_lifecycle.{h,cpp}`: take-once stop,
  breaker-reset, and profile-switch requests plus the supervisor→worker
  profile-cycle signal),
- read-loop and write-once mode entry points
  (`read_loop.cpp`, `write_orchestrator.cpp`).

`hardware/`

- AMD reader,
- GPU reader,
- fan writer,
- simulation backends,
- vendored backend adapters,
- PawnIO module-binary resolver, loader, and SHA-256 verifier
  (`pawnio_binary.{h,cpp}`); upstream provenance for the vendored bin is
  recorded in `third_party\pawnio\README.md`.

`platform/`

- cross-process named mutex (`runtime_singleton.{h,cpp}`),
- environment and ISO-8601 helpers (`env_util.{h,cpp}`, `runtime_util.{h,cpp}`),
- direct runtime sampling (`direct_runtime_snapshot.{h,cpp}`),
- Win32 handle wrappers,
- mutex/driver-handle RAII,
- shared streaming SHA-256 helper (`file_hash.{h,cpp}`,
  `Sha256FileHex`) consumed by the analyze report and the CSV archive,
- read-only feasibility probe (`service_probe.{h,cpp}`),
- machine-id resolution from host name and optional override file
  (`machine_identity.{h,cpp}`: `ResolveMachineId`),
- task runner binary (`task_runner.cpp`).

`policy/`

- curve lookup,
- temperature blending,
- curve shape parsing.

Rate limiting, demand smoothing, and boost-stage trim math live in
`control/` next to the channel evaluator they feed (see
`control_math.{h,cpp}`, `boost_stage.{h,cpp}`, `channel_evaluator.cpp`).

## Migration Order

Completed:

1. Added `svg_mb_control_core` and moved non-app source files into the library.
2. Kept `svg-mb-control.exe` as a thin target linking `svg_mb_control_core`.
3. Moved implementation files into responsibility directories.
4. Added a CTest target for core smoke coverage.
5. Wired CTest into the release build before the Python hermetic suite.
6. Split `control_loop.cpp` (was 1116 lines) into focused modules:
   `cadence_score`, `low_band_integrator`, `channel_write`, and
   `tick_runner`. The residual `control_loop.cpp` (~283 lines) holds only
   the `ControlLoop` public surface and startup/shutdown choreography.
7. Extracted PawnIO binary resolution, file load, and SHA-256 verification
   from `amd_reader.cpp` into the standalone `hardware/pawnio_binary`
   module, and registered a dedicated CTest target
   (`svg_mb_control_pawnio_binary_tests`) for the pure helpers.
8. Replaced four hand-rolled per-tick boost integrators with a
   table-driven `UpdateBoostStage` in `control/boost_stage.{h,cpp}`
   driven by `kBoostStageSpecs`. Deleted 21 legacy config fields from
   `ChannelControlConfig` and four legacy state doubles from
   `ChannelState`. CSV/JSON/banner output stayed byte-identical.
   Trajectory equivalence is locked by
   `svg_mb_control_boost_stage_tests`.
9. Moved the shared math primitives (`SmoothStep`, `SmoothScale`,
   `MoveTowardRateLimited`) from `cadence_score.cpp` into
   `control/control_math.{h,cpp}`. `cadence_score` keeps just the
   cadence logic; `low_band_integrator` consumes the primitives without
   pulling in `cadence_score.h`.
10. Re-homed `runtime_write_policy.{h,cpp}` and
    `write_orchestrator.{h,cpp}` from `src/policy/` to `src/runtime/`.
    `src/policy/` now contains only the curve/blend math the channel
    evaluator consumes.
11. Split `EvaluateChannel` into five single-purpose helpers
    (`EvaluatePrimarySetpoint`, `ApplyCpuOverride`,
    `UpdateDemandAndBoosts`, `ComputeFinalSetpoint`,
    `DetectAuthorityReassert`) threaded through a local
    `EvaluationScratch` shim. The orchestrator dropped from ~140 lines
    to ~30.
12. Split `src/analyze/analyze_report.cpp` (was 845 lines) into a
    `RunAnalyzeReport` orchestrator plus three sibling modules in the
    `svg_mb_control::analyze::report_detail` namespace:
    `analyze_report_data.{h,cpp}`, `analyze_report_queries.{h,cpp}`,
    `analyze_report_emit.{h,cpp}`. The new `analyze report` flow also
    writes JSON analysis manifests and Markdown decision records via
    the new `platform/file_hash` SHA-256 helper.
13. Added three CTest targets covering pure helpers that recent
    refactors exposed: `svg_mb_control_math_tests` (smootherstep + rate
    limit), `svg_mb_control_analyze_report_tests` (percentile / band
    summary / band assignment / diagnostic flags), and
    `svg_mb_control_loop_config_tests` (boost-stage validator round
    trips through `LoadControlLoopConfig`).
14. Split `src/app/app_main.cpp` (was 847 lines) into a thin `RunApp`
    orchestrator plus three sibling modules: `app/app_signals.{h,cpp}`
    (Win32 console handler + RAII scopes, exposes `StopSignaled()`),
    `app/app_args.{h,cpp}` (`CliOptions` + `ParseCliOptions` + value
    parsers + `PrintUsage`/`PrintVersion`), and `app/app_diagnose.{h,cpp}`
    (`RunDiagnoseAmd`, `RunDiagnoseGpu`, `SampleDirectSnapshotJson`).
15. Converted repeated runtime CSV field groups in
    `runtime_csv_rows.cpp` to descriptor tables shared by header and row
    builders. `svg_mb_control_csv_rows_tests` now checks read-loop,
    evidence-log, and control-loop header/row alignment plus representative
    present/absent indexed values.
16. Collapsed the duplicated core-linked CTest registration blocks in
    `CMakeLists.txt` into one `svg_mb_control_add_core_test(name source)`
    helper plus one-line calls. The two header-only tests
    (`svg_mb_control_rapl_energy_tests`, `svg_mb_control_cpu_cycles_tests`)
    stay hand-rolled because they must not link `svg_mb_control_core`. The
    current build graph registers twenty core-linked CTest targets through that
    helper plus the two hand-rolled header-only tests, all verified by
    `Test-LocalCI`.

Remaining polish:

1. Convert includes to module-qualified paths only if the team wants stricter
   include ownership; the current build keeps compatibility include roots.
2. Split Python smoke tests by runtime mode if the single file becomes hard to
   navigate.
3. Separate build/package from live deploy/restart if local verification should
   never stop the controller.

## Guardrails

- Do not reintroduce bridge processes or sibling-runtime dependencies.
- Do not move vendored third-party source into first-party modules.
- Do not change controller behavior during file moves unless tests and run data
  justify it.
- Keep `control_runtime.json`, `current_state.json`, `pending_writes.json`, CSV,
  and JSONL schemas explicit when moving runtime code.
- Prefer small compatibility-preserving moves over a large namespace rewrite.
