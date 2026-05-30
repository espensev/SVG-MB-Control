# Control Simplification Targets

Status: completed simplification record, last updated 2026-05-29. The original
2026-05-26 target list is fully closed (per-target Status lines below).

This note records behavior-preserving simplification targets for the control
runtime. The intent is to reduce expression complexity and duplicated control
logic without changing shipped tuning, channel curves, boost rates, write
cadence, runtime sidecar contracts, or live fan behavior.

For future behavior-preserving simplification work, use
`docs/CONTROL_PIPELINE_MATH.md` as the identity reference. Any implementation
change that alters curve lookup, smoothing, boost composition, low-band
behavior, cadence scoring, CSV/status fields, or response attribution must
update that reference and run the normal validation workflow.

## Constraints

- Keep the repo standalone and direct-only.
- Do not change tuning defaults as part of these simplification passes.
- Preserve upsert-before-write ordering for pending write sidecars.
- Preserve status, health, CSV, JSONL, manifest, and low-band evidence fields
  unless a separate schema change is explicitly scoped.
- Prefer small commits that can be reviewed against the math identities.
- For C++ behavior changes, use `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`.

## 1. Unify The Math Primitives

Status: **completed 2026-05-28**. `SmoothStep`, `SmoothScale`, and
`MoveTowardRateLimited` now live in `src/control/control_math.{h,cpp}`;
`cadence_score` and `low_band_integrator` include it. C++ coverage:
`tests/cpp/control_math_tests.cpp` (svg_mb_control_math_tests target).
`channel_evaluator::RateLimitSetpoint` stayed where it is: it has two
semantics MoveTowardRateLimited does not (NaN-current → return target
for the first-write contract, plus the additive `max_setpoint_step_pct`
cap). `policy::control_policy.cpp` retains its own SmootherStep for the
curve-shape interpolation only.

Original analysis (kept for context):

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

Status: **completed 2026-05-28**. `EvaluateChannel` is now a 30-line
orchestrator threading partial state through a local `EvaluationScratch`
shim across five private helpers: `EvaluatePrimarySetpoint`,
`ApplyCpuOverride`, `UpdateDemandAndBoosts`, `ComputeFinalSetpoint`,
`DetectAuthorityReassert`. The boost-sum operand order in
`ComputeFinalSetpoint` is preserved bit-for-bit so FP associativity
stays identical to the pre-refactor expression.

Original analysis (kept for context):

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

Status: **completed 2026-05-28**. `UpdateLowBandState` now reads
through named predicates: `SensorReleased(available, temp_c,
release_c)`, `PrimaryResponseActive(channels)` (carries the one-tick
lag note in its docstring), `ShouldAccrueDebt(signal, primary_active)`,
`ShouldReleaseDebt(cpu_released, gpu_released)`, and
`StageSpacingSatisfied(state, spacing, now)`. The signal-active and
primary-response-freeze thresholds are now named constexpr constants
(`kSignalEpsilonPct`, `kPrimaryResponseFreezeThresholdPct`).
`LowBandChannelConfigured` was already centralized. State mutation
order and the deliberate one-tick lag are unchanged.

Original analysis (kept for context):

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

Status: **completed pre-2026-05-28** in commit "Drive run-mode
label/argument/parse from a single table" (3bf3e38). `kRunModeTable` in
`src/control/control_supervisor.cpp` is now the single source of truth
for the six mode strings; both `ParseRunMode` overloads, `RunModeLabel`,
and `RunModeArgument` adapt the same table.

Original analysis (kept for context):

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

Status: **closed 2026-05-28; no further action planned**. `src/app/app_main.cpp`
was split into a thin `RunApp` orchestrator plus three sibling modules:
`app/app_signals.{h,cpp}` (Win32 handler + RAII scopes,
`StopSignaled()`), `app/app_args.{h,cpp}` (`CliOptions`,
`ParseCliOptions`, the value parsers, `PrintUsage`, `PrintVersion`), and
`app/app_diagnose.{h,cpp}` (`RunDiagnoseAmd`, `RunDiagnoseGpu`,
`SampleDirectSnapshotJson`). The `ParseCliOptions` if/else-if ladder
itself remains in `app_args.cpp` intentionally: it is linear, explicit, and
keeps value-taking options close to their validation. Grouped parse helpers
would mostly move branches sideways without improving the operator-facing
logic.

Original analysis (kept for context):

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

Status: **completed 2026-05-29**. Every repeated per-index field group in
`src/runtime/runtime_csv_rows.cpp` is now descriptor-driven. The
boost-stage channel columns still come from the shared `kBoostStageSpecs`
loop; the fan-snapshot, fan tach evidence, SIO voltage, SIO temperature,
and remaining per-channel columns now come from `CsvColumn<Entity>` tables
(`kFanSnapshotColumns`, `kFanTachEvidenceColumns`, `kSioVoltageColumns`,
`kSioTemperatureColumns`, `kChannelPreBoostColumns`,
`kChannelPostBoostColumns`). Each entry pairs a column-name suffix with its
row writer, and both the header builder and the row builder iterate the
same table, so the two cannot drift. Emitted column names and values are
byte-identical; the GPU-evidence block in `gpu_evidence_csv.cpp` was
already a paired header/row helper and is left as is. C++ coverage:
`tests/cpp/csv_rows_tests.cpp` (svg_mb_control_csv_rows_tests target)
asserts header-column-count == row-field-count for all three CSV variants
and spot-checks boundary column names plus representative present/absent row
values; the end-to-end `tests/test_read_loop.py`,
`tests/test_evidence_log.py`, and `tests/test_control_loop.py` continue to lock
the real header names against the by-name consumers (native analyze ingest and
`tools/eval_dashboard/dashboard.js`).

Original analysis (kept for context):

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

## 7. Build Test CSV Fixtures From Field Descriptors

Status: **completed 2026-05-28**. `tests/test_analyze_ingest.py` now builds
fixture headers and rows from field lists (`COMMON_FIELDS`,
`FAN_FIELD_SUFFIXES`, `LOOP_FIELDS`, `CHANNEL_FIELD_SUFFIXES`) and routes rows
through `_csv_row`, which rejects missing or extra fields before writing the
CSV.

Original shape (kept for context):

- `tests/test_analyze_ingest.py` owns `CSV_HEADER_PARTS`, `_write_fixture_csv`,
  and `_write_ramp_csv` as manually aligned string/cell lists.
- Adding a channel CSV field requires updating the header and every fixture row
  in lockstep.
- A missing cell shifts every later column while still producing parseable CSV,
  so failures surface indirectly in analyzer assertions.

Simplification:

- Replace the hand-aligned fixture row lists with a small descriptor-driven
  builder local to the test file.
- Keep the fixture values explicit, but map them by field name:
  - common row fields,
  - fan fields,
  - loop timing fields,
  - channel fields.
- Have the builder assert that every generated row has exactly the same field
  count as the generated header before writing the CSV.

Payoff:

- Prevents header/row drift in analyzer fixtures.
- Keeps schema additions from becoming brittle column-position surgery.
- Gives tests clearer failure messages when a field is missing.

Risk:

- Low. This is test-only and does not touch runtime behavior.
- Keep the generated header order identical to the current fixture header.

Validation:

- Run `python -m unittest tests.test_analyze_ingest -v`.
- Run local CI when paired with runtime/analyzer schema changes.

## 8. Add A Staged Background Runtime Test Helper

Status: **completed 2026-05-28**. `tests/helpers.py::StagedControlApp` now
owns staged executable setup, read-loop config creation, runtime-home paths,
start/status/restart/stop helpers, and guaranteed cleanup for background
supervisor/worker tests. `tests/test_smoke.py` uses it for staged launch,
restart, watchdog-start, and startup-failure coverage.

Original shape (kept for context):

- `tests/test_smoke.py` repeats the same staged-executable setup:
  - create temp dir,
  - copy `svg-mb-control.exe`,
  - write a read-loop config,
  - launch with zero args,
  - clean up with `--stop` in a `finally` block.
- These are not the same as `RuntimeProbe`: they exercise the background
  supervisor/worker launch path rather than a directly spawned foreground
  process.

Simplification:

- Add a small helper or context manager, for example
  `StagedControlApp`, that owns:
  - staged executable path,
  - runtime home,
  - default read-loop config creation,
  - `start`, `stop`, `status`, and `restart` helpers.
- Keep the explicit test assertions in `test_smoke.py`; only centralize setup
  and guaranteed stop cleanup.

Payoff:

- Removes repeated staging boilerplate from the longest smoke tests.
- Makes background-process cleanup more reliable.
- Keeps foreground `RuntimeProbe` and background supervisor tests distinct.

Risk:

- Low-medium. These tests intentionally exercise process lifetime, so the helper
  must not hide failed start/stop return codes.
- Preserve tests that deliberately corrupt startup state before launch.

Validation:

- Run `python -m unittest tests.test_smoke -v`.
- Confirm no lingering `svg-mb-control.exe` test children remain afterward.

## 9. Split `RunAnalyzeReport` Into Query And Assembly Helpers

Status: **completed 2026-05-28**. The 845-line `analyze_report.cpp`
became a ~180-line `RunAnalyzeReport` orchestrator plus three sibling
modules in the `svg_mb_control::analyze::report_detail` namespace:
`analyze_report_data.{h,cpp}` (types + percentile/median/SummariseBand
helpers), `analyze_report_queries.{h,cpp}` (`LoadTicks`,
`ComputeElapsedTime`, `AssignBands`, `LoadChannelStats`, `MergeFanStats`,
`LoadRobustnessCounts`, `LoadEventFieldCounts`,
`ComputeIdleSetpointBaselines`, `DetectResponseDelay`,
`LoadRuntimeManifestEvidence`), and `analyze_report_emit.{h,cpp}`
(`EmitJsonReport`, `EmitTextReport`, `BuildDecisionRecord`,
`WriteAnalysisManifest`, `BuildDiagnosticFlags`,
`AutoDecisionRecordPath`, `WriteTextFile`). C++ coverage:
`tests/cpp/analyze_report_tests.cpp` (svg_mb_control_analyze_report_tests
target).

Original analysis (kept for context):

- `src/analyze/analyze_report.cpp::RunAnalyzeReport` is a large pipeline that
  opens the database, selects the run, loads ticks, assigns bands, loads
  channel samples, loads fan samples, counts events, computes baselines, detects
  response delay, and emits output.
- The JSON and text emitters are already separate; the remaining complexity is
  in data loading and report assembly.

Simplification:

- Extract behavior-preserving helpers:
  - `LoadTicks(db, run_id)`.
  - `AssignBands(ticks, options)`.
  - `LoadChannelStats(db, run_id)`.
  - `MergeFanStats(db, run_id, channels)`.
  - `LoadRobustnessCounts(db, run_id)`.
  - `DetectResponseDelay(db, run_id, ticks, options)`.
- Keep `ReportData` as the handoff object to the existing emitters.

Payoff:

- Makes analyzer failures easier to localize.
- Reduces positional SQL-column fragility by giving each query one narrow
  mapping site.
- Opens the door to focused tests around banding and response detection.

Risk:

- Medium-low. This is offline analysis, not live control.
- Preserve current output fields and percentile behavior exactly.

Validation:

- Run `python -m unittest tests.test_analyze_ingest -v`.
- Run local CI if the refactor touches schema, ingest, or report output.

## 10. Centralize Analyzer Channel Sample Columns

Status: **completed 2026-05-28**.
`src/analyze/analyze_channel_sample_columns.{h,cpp}` now owns the
`tick_channel_samples` descriptor: column names, SQLite types, create-table SQL,
insert SQL, select-all SQL, CSV field-name construction, select indexes, and
`BindTickChannelSample`. `analyze_csv.cpp`, `analyze_db.cpp`,
`analyze_ingest_db.cpp`, and `analyze_report_queries.cpp` consume that
descriptor instead of hand-copying the channel field order.

Original shape (kept for context):

- A channel CSV field can touch several analyzer layers:
  - `src/analyze/analyze_csv.{h,cpp}`,
  - `src/analyze/analyze_db.cpp` DDL and migrations,
  - `src/analyze/analyze_ingest_db.cpp` insert SQL and bind positions,
  - `src/analyze/analyze_report_queries.cpp` query column positions,
  - wrapper/native analyzer fixtures.
- This is related to the runtime CSV mirroring target, but it is specifically
  the ingestion/database side.

Simplification:

- Start with the `tick_channel_samples` column group only.
- Introduce a small local descriptor or constants block for field names and
  bind/query positions.
- Prefer a narrow helper that binds a `ParsedChannelSample` to a prepared
  statement over a broad ORM-like abstraction.

Payoff:

- Reduces schema addition risk.
- Makes migrations and inserts easier to audit.
- Prevents positional bind mistakes when optional fields are inserted in the
  middle of the channel schema.

Risk:

- Medium. Database schema and migrations are durable contracts.
- Keep migrations backward-compatible and do not rename existing columns.

Validation:

- Run analyzer ingest/report tests.
- Verify migration tests or add one if an older schema fixture is introduced.

## 11. Reuse Control Config Channel Descriptors In Text And JSON Output

Status: **closed 2026-05-28; no further action planned**. The three BelowStart
pressure stages (thermal, midband, GPU airflow) now render their JSON
sub-objects via a `kBoostStageSpecs` loop in `BuildJsonSummary`, with JSON keys
coming straight from the spec table. The text summary uses a small
`kPressureBoostTextLabels` table that maps display labels to `BoostStage`.
The CPU low-soak block stays bespoke for text and JSON intentionally because
its shape differs: explicit `release_c`, per-minute rates, and different
operator-facing labels. Forcing it into the BelowStart descriptor would add
branching without reducing schema risk.

Original analysis (kept for context):

- `src/control/control_config_print.cpp` renders channel configuration twice:
  - operator text output,
  - JSON summary output.
- Pressure-style blocks are already descriptor-driven in config parsing, but
  summary rendering repeats thermal, midband, GPU airflow, CPU low-soak, and
  low-band output shapes by hand.

Simplification:

- Reuse a small descriptor list for staged channel response blocks:
  - display label,
  - JSON key,
  - start/full/release fields where applicable,
  - rise/fall/max fields.
- Keep bespoke text wording where it helps operators, but centralize the field
  selection and disabled/null logic.

Payoff:

- Keeps `--show-config` text and JSON aligned.
- Makes new channel response fields less likely to be omitted from one output.
- Builds on the existing `PressureBoostMembers` pattern from config parsing.

Risk:

- Low-medium. This is presentation-only, but smoke/config tests check output.
- Preserve current JSON keys and important text labels.

Validation:

- Run `python -m unittest tests.test_smoke tests.test_config_contracts -v`.

## 12. Retire The Python Analysis Renderer Split

Status: **closed 2026-05-29; no further action planned**.
`scripts/analyze_control_run.py` is now a thin compatibility wrapper: it ingests
a raw CSV into a temporary SQLite database through native
`svg-mb-control.exe analyze ingest --csv` and forwards native
`analyze report` output. The former Python renderers
(`render_markdown`, `build_decision_record`, `build_diagnostic_flags`, manifest
generation, GPU-response summaries, timing summaries) no longer exist, so
splitting them is obsolete.

Validation: `tests/test_analyzer.py` now checks wrapper integration, and
native report behavior is covered by `tests/test_analyze_ingest.py` plus the
C++ analyze-report tests.

## Suggested Order

Completed or closed:

1. Unify math primitives.
2. Extract raw demand and final setpoint composition from `EvaluateChannel`.
3. Name low-band gates.
4. Make run-mode parsing table-driven.
5. Build test CSV fixtures from field descriptors.
6. Centralize analyzer channel sample columns.
7. Add a staged background runtime test helper.
8. Split `RunAnalyzeReport` into query and assembly helpers.
9. Reuse control config channel descriptors in text and JSON output
   (closed with CPU low-soak intentionally bespoke).
10. Reduce CLI parser ladder pressure (closed with the residual ladder
    intentionally explicit).
11. Reduce CSV schema/header mirroring (closed by making every repeated
    per-index CSV field group descriptor-driven; see target 6).
12. Retire the Python analysis renderer split (closed because
    `scripts/analyze_control_run.py` now delegates analysis to native
    `analyze ingest --csv` plus `analyze report`; see target 12).

Remaining:

None. All targets are completed or closed.
