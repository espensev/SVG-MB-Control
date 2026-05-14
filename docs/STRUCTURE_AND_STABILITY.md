# Structure and Stability Plan

## Direction

`SVG-MB-Control` should stay as one standalone runtime repo. The long-term
stability improvement is not another repo split; it is a clearer internal module
layout and a testable core library.

Target layout:

```text
src/
  app/        main, CLI parsing, mode dispatch
  control/    loop orchestration, channel evaluator, status model
  runtime/    runtime store, CSV logger, event log, JSON IO
  hardware/   AMD, GPU, SIO fan backends
  platform/   Windows timer, process metrics, HANDLE wrappers
  policy/     curves, blending, rate limits, demand smoothing
```

Add a `svg_mb_control_core` static library target. The existing
`svg-mb-control.exe` target should become a thin app layer that links that
library.

## Why

The current repo has the right ownership boundary: one executable owns telemetry,
policy, runtime state, writes, restore, and logs. The main structural risk is
that implementation responsibilities are concentrated in large translation
units, especially the control loop and smoke test file.

A core library gives three stability benefits:

- control logic can be unit-tested without launching the executable,
- runtime IO and hardware IO can be mocked or replaced behind small interfaces,
- future refactors can move files without changing the product boundary.

## Module Responsibilities

`app/`

- command-line parsing,
- config path selection,
- mode dispatch,
- console control handling,
- process startup output.

`control/`

- control-loop orchestration,
- channel evaluation,
- write decision model,
- authority/reassert logic,
- failure-state model,
- control status publication shape.

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
- vendored backend adapters.

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

1. Finish behavior-preserving cleanup already in progress:
   - remove dead JSON scanner helpers,
   - add AMD PCI mutex RAII,
   - expose live failure-state fields,
   - add the run analyzer.
2. Add `svg_mb_control_core` and move existing source files into the library
   without changing behavior.
3. Move files into responsibility directories in small batches.
4. Extract pure channel evaluation and policy tests first.
5. Split Python smoke tests by runtime mode after the core library exists.
6. Separate build/package from live deploy/restart so local verification does
   not implicitly stop the controller.

## Guardrails

- Do not reintroduce bridge processes or sibling-runtime dependencies.
- Do not move vendored third-party source into first-party modules.
- Do not change controller behavior during file moves unless tests and run data
  justify it.
- Keep `control_runtime.json`, `current_state.json`, `pending_writes.json`, CSV,
  and JSONL schemas explicit when moving runtime code.
- Prefer small compatibility-preserving moves over a large namespace rewrite.
