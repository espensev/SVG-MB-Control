# Discovery - Controller Response and Code Structure

**Goal:** Map and review the controller response and the structure of the code.
**Date:** 2026-06-26
**Status:** complete
**Recommended next:** none - standalone review. Optional follow-up is a small
behavior-preserving cleanup pass around response label constants and tick-runner
extraction.

---

## Questions

1. What is the control-loop entry path and how is ownership split?
2. How is the per-channel controller response computed?
3. How does a computed response become a write, event, status row, and CSV row?
4. How does offline analysis consume response attribution?
5. Does the current source structure match the documented module boundaries?
6. What review findings or risks are visible from the map?

---

## Findings

### Q1: What is the control-loop entry path and how is ownership split?

**Answer:** The control-loop entry path is thin at the executable edge and
passes into focused control/runtime components. The top-level flow is:

```text
src/main.cpp
  -> src/app/app_main.cpp::RunApp
  -> LoadControlConfig / LoadControlLoopConfig
  -> ControlLoop::RunUntilStopped
  -> RunControlTick
  -> channel controller Evaluate(...)
  -> TryApplyChannelSetpoint
  -> runtime status / CSV / event surfaces
  -> analyzer ingest/report for offline summaries
```

`app_main.cpp` still owns a lot of CLI orchestration, but the actual long-running
control path is no longer embedded there. `control_loop.cpp` owns startup,
shutdown, CSV logger initialization, hardware backend initialization, status
startup/shutdown publication, and the tick loop. The steady-state tick body is
delegated to `tick_runner.cpp`.

**Evidence:**
- `src/main.cpp:4` forwards directly to `svg_mb_control::RunApp`.
- `src/app/app_main.cpp:75-81` begins `RunApp` and parses CLI options.
- `src/app/app_main.cpp:445-459` loads `ControlLoopConfig`, resolves runtime
  write policy, constructs `ControlLoop`, and calls `RunUntilStopped`.
- `src/control/control_loop.cpp:72-74` documents that per-tick sampling,
  decisions, and writes live in `tick_runner.cpp`.
- `src/control/control_loop.cpp:270-279` loops on stop/profile-cycle requests
  and calls `RunControlTick`.

**Implications:**
- The top-level ownership is understandable: CLI dispatch in `app`, lifecycle in
  `control_loop`, tick orchestration in `tick_runner`, response math in
  controller/evaluator files, and persistence in `runtime`.
- Future control-response work should not bypass `RunControlTick` or write
  directly from `app_main.cpp`.

### Q2: How is the per-channel controller response computed?

**Answer:** The curve-overlay response is a staged computation:

1. Select primary temperature input from CPU, GPU, max, or source-aware max.
2. Detect sensor failure/dropout and enter safe-mode response when needed.
3. Let the CPU override curve win when it commands a higher duty.
4. Apply demand smoothing.
5. Integrate boost overlays: thermal pressure, midband pressure, GPU airflow,
   CPU low soak, and low-band stage.
6. Apply low-band residual cap, clamp to `[0, 100]`, and rate-limit the final
   setpoint.
7. Detect authority reassert for continuous-hold channels.

PID uses the same primary-temperature selection and reporting model, but tags
the response as `pid` and can set `write_suppressed` for shadow/dry-run mode.

**Evidence:**
- `src/control/channel_evaluator.cpp:142-219` selects the primary curve input,
  handles sensor failures/dropout, and seeds `response_source`.
- `src/control/channel_evaluator.cpp:223-237` applies `cpu_override_curve` and
  changes `response_source` to `cpu_override` when it wins.
- `src/control/channel_evaluator.cpp:243-272` applies demand smoothing and boost
  stages, then appends response-source modifiers.
- `src/control/channel_evaluator.cpp:280-308` applies low-band residual cap,
  sums boost terms, clamps the desired setpoint, and rate-limits it.
- `src/control/channel_evaluator.cpp:487-520` is the public `EvaluateChannel`
  orchestration.
- `src/control/pid_controller.cpp:124-206` computes PID responses, writes PID
  evidence into `ChannelState`, and tags live/shadow PID rows consistently.

**Implications:**
- The response model is coherent and reviewable. The code already reads like a
  control pipeline rather than one monolithic formula.
- `response_source` means "computed source/contributors for this channel state",
  not "a hardware write definitely occurred this tick".

### Q3: How does a computed response become a write, event, status row, and CSV row?

**Answer:** The write path is intentionally downstream of response computation.
`TryApplyChannelSetpoint` can return without writing because of dry-run PID,
deadband, cooldown, missing baseline, runtime policy, write breaker state, or
missing effective write authorization. Successful writes update channel state,
set `last_write_reason`, and emit `control_loop.write_applied`; failures emit
write/breaker events and leave an observable failure state.

Status and CSV do not re-run the controller calculation. They read the latest
`ChannelState` through a single `RuntimeControlChannelLogState` bridge.

**Evidence:**
- `src/control/tick_runner.cpp:337-376` captures baseline, restores expired
  holds, evaluates each controller, emits sensor events, and calls
  `TryApplyChannelSetpoint`.
- `src/control/channel_write.cpp:269-307` returns before writing for missing
  setpoint, dry-run PID, deadband, cooldown, missing baseline, or policy gates.
- `src/control/channel_write.cpp:316-358` suppresses normal writes behind an
  open breaker except bounded cooling-demand probes; safe-mode writes bypass
  that gate.
- `src/control/channel_write.cpp:412-458` applies the duty, updates write state,
  records `last_write_reason`, and emits `control_loop.write_applied`.
- `src/control/control_status_writer.cpp:7-48` copies `ChannelState` into
  `RuntimeControlChannelLogState`.
- `src/runtime/runtime_status.cpp:141-143` publishes last response, primary
  source, and write reason into `control_runtime.json`.
- `src/runtime/runtime_csv_rows.cpp:352-365` emits the same fields into CSV.

**Implications:**
- The separation between computed response and actuation is correct for safety,
  PID shadowing, policy gates, and analysis.
- Dashboards and reports should compare `response_source` with `write_reason`
  and `total_writes`; `response_source` alone is not enough to prove actuation.

### Q4: How does offline analysis consume response attribution?

**Answer:** Offline analysis consumes `primary_temp_source`, `response_source`,
and `write_reason` as structured columns, not by scraping event detail strings.
The analyzer stores them in `tick_channel_samples`, counts source/reason
frequencies per channel, and emits them in text, JSON, and decision-record
outputs.

**Evidence:**
- `src/analyze/analyze_channel_sample_columns.cpp:28-43` declares
  `primary_temp_source`, `response_source`, `write_reason`, `feedforward_pct`,
  and `correction_pct` as channel-sample columns.
- `src/analyze/analyze_csv.cpp:129-136` parses those fields by descriptor.
- `src/analyze/analyze_report_queries.cpp:828-864` counts primary source,
  response source, and write reason labels, treating blank/null as
  `unavailable`.
- `src/analyze/analyze_report_emit.cpp:554-559` emits the count summaries in
  text reports.
- `src/analyze/analyze_report_emit.cpp:781-790` emits channel attribution in
  generated decision records.

**Implications:**
- The current analyzer contract supports response review well.
- Any change to response/write label spelling is a telemetry/schema change in
  practice, even if the CSV column names stay stable.

### Q5: Does the current source structure match the documented module boundaries?

**Answer:** Yes, with a few known bulk hotspots. The source matches the repo
contract: `app` dispatch, `control` response and lifecycle, `runtime` sidecars
and logs, `hardware` readers/writers, `platform` process/Win32 helpers, `policy`
curve math, and `analyze` offline reporting. The build reinforces this by
putting non-app implementation into `svg_mb_control_core` and registering
focused C++ tests against it.

**Evidence:**
- `README.md` and `docs/STRUCTURE_AND_STABILITY.md` both define the same module
  split.
- `CMakeLists.txt:143-205` builds `svg_mb_control_core` from the first-party
  control/runtime/hardware/platform/policy/analyze sources.
- `CMakeLists.txt:363-405` registers focused core-linked C++ tests, including
  CSV rows, runtime status/event log, channel write, channel controller, PID,
  and force terminate tests.
- A size scan found the largest active files are now analyzer/report/runtime
  surfaces and orchestration files, with `src/control/tick_runner.cpp` at about
  538 lines and `src/control/channel_evaluator.cpp` at about 479 lines.

**Implications:**
- The earlier monolith risk has been reduced: the core control-loop lifecycle,
  channel evaluation, write decision, status emission, and CSV emission are
  separate files.
- `tick_runner.cpp` remains the main control hot spot because it coordinates
  sampling, low-band, snapshot publication, channel decisions, writes, sidecar
  flush, timing, CSV, evidence, and status retry.

### Q6: What review findings or risks are visible from the map?

**Answer:** I did not find a high-severity correctness bug in the controller
response path from this code review. The risks are structural and contract
oriented.

**Evidence and findings:**

| Severity | Finding | Evidence | Recommendation |
|---|---|---|---|
| Medium | GPU-envelope identity is intentionally duplicated across control, analyzer, scripts, and dashboard code. | `src/control/channel_evaluator.cpp:442-449` names the other copies; `rg GpuEnvelopeC/gpuEnvelope/gpu_envelope` finds control, analyzer, dashboard, tests, and scripts. | Do not change this rule casually. If it changes, update every named copy plus tests in the same change, or extract a shared documented descriptor where practical. |
| Medium | `response_source`, `primary_temp_source`, and `write_reason` are public telemetry labels but are still mostly string literals. | `src/control/channel_evaluator.cpp:167-235`, `src/control/pid_controller.cpp:149-205`, `src/control/channel_write.cpp:16-20`, and analyzer/tests all depend on exact spellings. | Add central constants or small enums with serializer functions before the next response-label expansion. Keep emitted strings unchanged. |
| Low | `RunControlTick` is still broad enough to be a future coupling point. | `src/control/tick_runner.cpp:225-570` handles sampling, request handling, low-band, snapshot/status failure state, per-channel evaluation/write, sidecar flush, timing, CSV, evidence, and status. | If churn continues, extract pure helpers such as `BuildTempInputs`, `ProcessControlledChannels`, and `PublishTickArtifacts` without changing behavior. |
| Low | Response attribution can be misread as actuation attribution. | `src/control/channel_evaluator.cpp:513-520` sets the response before `src/control/channel_write.cpp:269-307` applies write gates; dry-run PID explicitly logs a setpoint without actuation in `src/control/channel_evaluator.h:47-53`. | Keep docs/reports explicit: response source explains demand origin; write reason and write counts explain actuation. |

**Implications:**
- The code is ready for targeted cleanup, not a broad restructure.
- Any behavior-changing response/control work still needs the repo's feature
  intake gate. The cleanup recommendations above can be behavior-preserving if
  emitted telemetry strings and schemas stay unchanged.

---

## Cross-Cutting Analysis

### Constraints

- The repo boundary is clean: runtime behavior is owned inside this repo and
  should not grow sibling-process adapters.
- `control_runtime.json`, control-loop CSV, JSONL events, analyzer SQLite
  columns, tests, and docs all depend on stable response/status field names.
- `control_runtime.json` is status-rate-limited; per-tick response analysis
  belongs in CSV/analyzer output.
- Feature specs remain required before new behavior, telemetry schema, CLI, live
  workflow, or shipped config behavior changes.

### Risks

| Risk | Likelihood | Impact | Notes |
|---|---:|---:|---|
| Response-label drift | Medium | Medium | A typo or renamed literal would fragment analyzer/report counts without a compiler error. |
| GPU-envelope rule drift | Medium | Medium | The rule is intentionally copied across consumers, so updates need a coordinated patch. |
| Tick-runner edit conflicts | Medium | Low | The file is an orchestration hotspot and likely to be touched by unrelated runtime/logging work. |
| Misreading response as write | Medium | Low | Current telemetry distinguishes the two, but reviews need to check both fields. |

### Open Questions

- None for this code map. I did not inspect live runtime behavior or current
  `release\runtime` evidence because the request was a code structure/review,
  and live runtime safety instructions forbid touching the running controller
  unless explicitly required.

---

## Recommendation

This does not need a multi-agent campaign. The current controller response path
is coherent and reasonably covered. If you want to act on the review, the best
next direct cleanup is:

1. Add centralized constants/serializers for `primary_temp_source`,
   `response_source`, and `write_reason` labels while preserving emitted strings.
2. Add a focused test that enumerates the public label vocabulary and confirms
   analyzer counting still accepts every label.
3. Extract one or two low-risk helpers from `RunControlTick` only after the
   label cleanup, so behavior-preserving review stays simple.

Validation for this discovery was read-only source/doc review. No build or test
run was needed because no product code changed.
