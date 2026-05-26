# Control Simplification Targets

Status: current planning note, 2026-05-26.

This note records behavior-preserving simplification targets for the control
runtime. The intent is to reduce expression complexity and duplicated control
logic without changing shipped tuning, channel curves, boost rates, write
cadence, runtime sidecar contracts, or live fan behavior.

Use `docs/CONTROL_PIPELINE_MATH.md` as the identity reference while doing this
work. Any implementation change that alters curve lookup, smoothing, boost
composition, low-band behavior, cadence scoring, CSV/status fields, or response
attribution must update that reference and run the normal validation workflow.

## Constraints

- Keep the repo standalone and direct-only.
- Do not change tuning defaults as part of these simplification passes.
- Preserve upsert-before-write ordering for pending write sidecars.
- Preserve status, health, CSV, JSONL, manifest, and low-band evidence fields
  unless a separate schema change is explicitly scoped.
- Prefer small commits that can be reviewed against the math identities.
- For C++ behavior changes, use `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`.

## 1. Unify The Math Primitives

Current shape:

- `src/control/cadence_score.cpp` owns `SmoothStep`, `SmoothScale`, and
  `MoveTowardRateLimited`.
- `src/policy/control_policy.cpp` owns a separate local `SmootherStep`.
- `src/control/channel_evaluator.cpp` owns a separate `RateLimitSetpoint`.
- The same conceptual operators are therefore spread across cadence, policy,
  and channel evaluation.

Simplification:

- Add a small shared math module, likely `src/control/control_math.{h,cpp}` or
  `src/policy/control_math.{h,cpp}`.
- Centralize:
  - `SmootherStep(double)`.
  - `SmoothScale(value, start, full)`.
  - generic move-toward/rate-limit logic.
  - small helpers such as `ClampPercent` only if they remove repeated intent.
- Keep wrapper names at call sites where they improve domain meaning, but have
  them delegate to one primitive implementation.

Payoff:

- Removes duplicated polynomial and rate-limit code.
- Gives unit tests a single target for numerical identities.
- Makes the math reference easier to keep aligned with source.

Risk:

- Low, if function semantics are copied exactly.
- Watch the difference between channel setpoint limiting, which supports
  `max_setpoint_step_pct`, and cadence/low-band limiting, which currently uses
  per-minute rates without a max-step cap.

Validation:

- Add or extend C++ smoke/unit tests for smootherstep endpoints, midpoint,
  clamping, disabled-rate identity, finite max-step cap, and NaN previous-value
  behavior.
- Run local CI after implementation.

## 2. Extract Raw Demand Resolution From `EvaluateChannel`

Current shape:

- `src/control/channel_evaluator.cpp::EvaluateChannel` handles:
  - effective timing computation,
  - missing primary sensor accounting,
  - sensor safe-mode entry/recovery,
  - primary curve lookup,
  - CPU override lookup,
  - demand smoothing,
  - boost integrator updates,
  - response-source attribution,
  - low-band residual cap,
  - final rate-limited setpoint,
  - authority reassert detection.

Simplification:

- Split the function into behavior-preserving helpers, for example:
  - `ResolveRawDemand(...)`: primary blend availability, sensor failure
    accounting, primary curve, safe-mode demand, CPU override, observed
    temperature, and base response source.
  - `UpdateChannelBoosts(...)`: thermal pressure, midband pressure, GPU
    airflow, and CPU low-soak state updates.
  - `ComposeFinalSetpoint(...)`: smoothing, additive boost composition,
    low-band residual cap, clipping, and final rate limit.
  - `UpdateAuthorityReassert(...)`: fan-state drift check against the computed
    setpoint.
- Keep the public `EvaluateChannel(...)` entry point so call sites and tests do
  not need to move all at once.

Payoff:

- The channel evaluator becomes readable as a pipeline.
- Raw demand, boost state, and output composition can be tested separately.
- Reduces the risk of future tuning edits accidentally changing sensor failure
  or response attribution behavior.

Risk:

- Medium-low. The function is hot-path behavior, but tests already cover many
  visible cases.
- The main risk is changing when `last_*` fields are updated on unavailable
  input or safe-mode transitions.

Validation:

- Add focused tests around CPU override, missing sensor safe mode, response
  source strings, low-band effective cap, and feedforward/correction identity.
- Run local CI after implementation.

## 3. Name Low-Band Gates

Current shape:

- `src/control/low_band_integrator.cpp::UpdateLowBandState` is compact, but key
  decisions are embedded as local predicates:
  - primary response active,
  - CPU/GPU released,
  - debt accrual,
  - debt release,
  - channel configured,
  - threshold eligibility,
  - stage spacing.

Simplification:

- Extract small named helpers such as:
  - `PrimaryResponseActive(channels)`.
  - `SensorReleased(available, temp_c, release_c)`.
  - `ShouldAccrueDebt(signal, primary_response_active)`.
  - `ShouldReleaseDebt(cpu_released, gpu_released)`.
  - `LowBandChannelConfigured(channel)`, if not already centralized enough.
  - `StageSpacingSatisfied(state, cfg, now)`.
- Keep the state mutation order unchanged: global debt first, then per-channel
  stage updates.

Payoff:

- Makes the code read like the math reference.
- Reduces future mistakes around the "second priority" debt freeze behavior.
- Keeps the low-band feature understandable without rereading all comments.

Risk:

- Low if helpers are pure predicates.
- Watch the deliberate one-tick lag: `PrimaryResponseActive` uses previous-tick
  boost state because low-band updates before per-channel evaluation.

Validation:

- Existing low-band smoke tests should continue to pass.
- Add one focused unit/smoke assertion if a helper changes an edge condition,
  especially deactivation hysteresis or stage spacing.

## 4. Make Run-Mode Parsing Table-Driven

Current shape:

- `src/control/control_supervisor.cpp` has two parallel `ParseRunMode`
  functions:
  - one accepts `const wchar_t*` for CLI mode parsing,
  - one accepts `std::string_view` for config `default_mode`.
- Both functions duplicate the same mode string ladder.

Simplification:

- Introduce a single mode table containing:
  - narrow name,
  - wide name or generated wide comparison,
  - `RunMode` enum value.
- Implement both overloads as thin adapters over the same table.
- Optionally add a `RunModeToString` helper if it removes future string
  duplication.

Payoff:

- One source of truth for mode names.
- Safer future mode additions.
- Small line-count and branch-count reduction.

Risk:

- Low.
- Preserve the two current error messages:
  - invalid CLI `--mode`,
  - invalid config `default_mode`.

Validation:

- Existing CLI and config contract tests should cover the main paths.
- Add a small C++ or Python test only if the refactor changes exposed errors.

## 5. Reduce CLI Parser Ladder Pressure

Current shape:

- `src/app/app_main.cpp::ParseCliOptions` is a long `if`/`else if` ladder that
  owns runtime commands, mode selection, write-once flags, calibration flags,
  help/version flags, diagnostics, and removed legacy option rejection.
- `RunApp` then has another command-dispatch ladder.

Simplification:

- Avoid a full command framework for now.
- Extract grouped parse helpers:
  - runtime command flags (`--start`, `--status`, `--health`, `--stop`,
    `--restart`, `--reset-breakers`, `--reset-breaker-channel`),
  - mode/write flags (`--mode`, `--write-channel`, `--write-pct`,
    `--write-hold-ms`),
  - calibration flags,
  - diagnostics/help/version,
  - removed legacy options.
- Consider a table only for flags with no value. Keep value-taking options
  explicit unless a table materially improves readability.
- Later, split command execution into small `Run*Command` helpers if dispatch
  remains hard to scan.

Payoff:

- Reduces the branchiest first-party file.
- Keeps CLI behavior easier to extend and review.
- Separates syntax parsing from command execution without introducing a new
  dependency or framework.

Risk:

- Medium. CLI parsing has many combinations and user-facing errors.
- Preserve current compatibility and exact enough diagnostics where tests rely
  on them.

Validation:

- Run existing smoke/config tests.
- Add focused tests for any command combination touched, especially `--json`
  constraints, reset-breaker exclusivity, `--start`, `--restart`, and
  write-once required fields.

## 6. Reduce CSV Schema/Header Mirroring

Current shape:

- `src/runtime/runtime_csv_rows.cpp` manually builds CSV headers and matching
  rows.
- Repeated per-channel fields appear in both header construction and row
  construction.
- The code is clear but schema-drift prone: adding a field requires updating
  matching header and row paths in the same order.

Simplification:

- Keep the current CSV format and field order.
- Introduce narrow helper functions for repeated field groups:
  - fan snapshot fields,
  - control-channel fields,
  - SIO voltage fields,
  - SIO temperature fields,
  - tach evidence fields.
- Avoid a broad schema engine unless field additions keep increasing.
- If a descriptor list is introduced, start with one repeated group only,
  then expand after proving the shape is clearer.

Payoff:

- Reduces header/row mismatch risk.
- Makes future telemetry additions less error-prone.
- Keeps CSV compatibility while improving local maintainability.

Risk:

- Medium-low. The behavior is pure formatting, but column order is a contract
  for tests, ingest, and dashboard consumers.

Validation:

- Existing CSV, ingest, analyzer, dashboard, read-loop, evidence-log, and
  control-loop tests should be run after implementation.
- For any descriptor-list approach, add a test that compares expected columns
  with parsed row keys.

## Suggested Order

1. Unify math primitives.
2. Extract raw demand and final setpoint composition from `EvaluateChannel`.
3. Name low-band gates.
4. Make run-mode parsing table-driven.
5. Reduce CSV schema/header mirroring.
6. Reduce CLI parser ladder pressure.

The first three are controller-local and should make the math easier to review.
The later three are broader maintainability work and can be done independently.

