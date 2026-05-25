# Structure and Stability Plan

## Current Structure

`SVG-MB-Control` should stay as one standalone runtime repo. The long-term
stability improvement is not another repo split; it is the clearer internal
module layout and testable core library now used by the build.

Current layout:

```text
src/
  app/        thin executable entry, CLI parsing, mode dispatch
  control/    loop orchestration, channel evaluator, status model
  runtime/    runtime store, CSV logger, event log, JSON IO
  hardware/   AMD, GPU, SIO fan backends
  platform/   Windows timer, process metrics, HANDLE wrappers
  policy/     curves, blending, rate limits, demand smoothing
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

- thin `wmain` wrapper,
- command-line parsing,
- config path selection,
- mode dispatch,
- console control handling,
- process startup output.

`control/`

- control-loop lifecycle (`control_loop.cpp`: startup, shutdown, dispatch),
- per-tick body (`tick_runner.cpp`: sampling, channel decisions, artifacts, wait),
- channel evaluation (`channel_evaluator.cpp`: curve, smoothing, pressure terms),
- channel-write gates (`channel_write.cpp`: deadband, cooldown, breaker, hold restore),
- adaptive cadence (`cadence_score.cpp`: slew score, effective tick interval),
- low-band integrator (`low_band_integrator.cpp`: signal, debt, stage activation),
- control status publication shape (`control_status_writer.cpp`),
- supervisor and run-mode dispatch (`control_supervisor.cpp`),
- config loading and validation (`control_loop_config.cpp`, `control_config.cpp`).

`runtime/`

- runtime-home path handling,
- atomic JSON writes,
- CSV logger,
- event JSONL writer,
- pending-write sidecars,
- run summary/manifest plumbing.

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

- Windows timer resolution,
- process resource sampling,
- Win32 handle wrappers,
- mutex/driver-handle RAII.

`policy/`

- curve lookup,
- temperature blending,
- rate limiting,
- demand smoothing,
- thermal-pressure trim math.

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

Remaining polish:

1. Split `src/app/app_main.cpp` further if CLI parsing or mode dispatch grows.
2. Convert includes to module-qualified paths only if the team wants stricter
   include ownership; the current build keeps compatibility include roots.
3. Split Python smoke tests by runtime mode if the single file becomes hard to
   navigate.
4. Separate build/package from live deploy/restart if local verification should
   never stop the controller.

## Guardrails

- Do not reintroduce bridge processes or sibling-runtime dependencies.
- Do not move vendored third-party source into first-party modules.
- Do not change controller behavior during file moves unless tests and run data
  justify it.
- Keep `control_runtime.json`, `current_state.json`, `pending_writes.json`, CSV,
  and JSONL schemas explicit when moving runtime code.
- Prefer small compatibility-preserving moves over a large namespace rewrite.
