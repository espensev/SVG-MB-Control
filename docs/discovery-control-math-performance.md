# Discovery - Control Math And Performance Opportunities

**Goal:** Find new behavior-preserving simplification and performance opportunities in the control path/math and the rest of the code; flag math/policy changes as evidence-gated proposals rather than applying them blind.
**Date:** 2026-05-30
**Status:** complete; first behavior-preserving cleanup batch implemented
**Recommended next:** if more performance work is needed, build the analyzer `CsvParsePlan`; keep policy/math tuning changes behind runtime evidence.

## Follow-up implementation - 2026-05-30

- Added a reusable `RuntimeSnapshotIndex` rebuilt once per sampled control tick. The control path now uses that index for CPU sensor lookup, baseline capture, authority reassert detection, write gating, low-band RPM evidence, and control-loop CSV fan fields.
- Centralized the remaining smootherstep polynomial copies through `SmoothStep` while preserving the boost-stage collapsed-window rule.
- Removed the intermediate escaped-string allocation from `AppendCsvString`; emitted CSV fields and escaping semantics remain unchanged.
- Added a `std::from_chars` fast path for analyzer double parsing with the previous `std::stod` behavior retained as fallback.
- Validation: `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` passed, including CTest and 114 hermetic Python tests.

---

## Questions

1. Where is the current control-loop math implemented, and which parts are pure behavior-preserving refactor candidates?
2. Which hot-path code repeats work per tick, per channel, or per log row?
3. Are there data-shape or descriptor opportunities similar to the recent CSV cleanup, but outside runtime CSV rows?
4. Which ideas would change control policy or numerical behavior and therefore need runtime evidence before implementation?
5. What tests already cover the risky surfaces, and what tests would be needed before changing them?
6. Which docs should hold the resulting queue so it stays separate from historical notes?

---

## Findings

### Q1: Where is the current control-loop math implemented, and which parts are pure behavior-preserving refactor candidates?

**Answer:** The maintained control identity is already split across focused modules. The remaining pure-code candidates are small: centralize the remaining smootherstep polynomial copies, then optionally generalize the two rate-limit helpers without changing first-write/max-step semantics.

**Evidence:**
- `docs/CONTROL_PIPELINE_MATH.md:3` - the math reference is current and names the source modules for tick orchestration, channel evaluation, boost stages, low-band, cadence, writes, and policy lookup.
- `src/control/control_math.cpp:8` - `SmoothStep` owns the shared clamped quintic polynomial.
- `src/control/boost_stage.cpp:38` - `PressureScale` still repeats the smootherstep polynomial, but has a distinct collapsed-window rule (`full_c <= start_c` returns full strength).
- `src/policy/control_policy.cpp:46` - curve lookup still has a local `SmootherStep` copy.
- `src/control/channel_evaluator.cpp:37` - `RateLimitSetpoint` duplicates the move-toward shape with two extra channel semantics: first-write NaN returns target, and `max_setpoint_step_pct` caps the per-tick step.

**Implications:**
- Implemented: `boost_stage.cpp` and `control_policy.cpp` now use `SmoothStep`; the boost-stage degenerate pressure-window branch stays explicit.
- A more generic rate-limit primitive is possible, but should be tested for NaN-current, zero-rate, zero-dt, and finite max-step behavior before replacing `RateLimitSetpoint`.

### Q2: Which hot-path code repeats work per tick, per channel, or per log row?

**Answer:** The clearest live hot-path opportunity is repeated linear lookup over the same per-tick `RuntimeSnapshot`. The current machine has six controlled channels and seven fan snapshots, so the cost is bounded, but it is still repeated in channel evaluation, write gates, low-band evidence, and CSV row emission.

**Evidence:**
- `src/runtime/runtime_snapshot.cpp:137` - `FindRuntimeFanChannel` linearly scans `snapshot.fans`.
- `src/runtime/runtime_snapshot.cpp:168` - `FindRuntimeAmdSensorTemperature` linearly scans `snapshot.amd_sensors`.
- `src/control/tick_runner.cpp:157` - each tick samples one runtime snapshot, then reuses it for every channel.
- `src/control/channel_write.cpp:23` and `src/control/channel_write.cpp:133` - write policy and baseline capture each search fan state by channel.
- `src/control/channel_evaluator.cpp:373` - authority reassert detection searches fan state again.
- `src/control/low_band_integrator.cpp:215` - low-band RPM evidence searches fan state once per channel.
- `src/runtime/runtime_csv_rows.cpp:449` - CSV control-channel row emission searches channel state by index.
- Live runtime read-only sample from `release\runtime\logs\svg_mb_control_output.csv`: last 399 rows had `loop_work_duration_ms` p50 `2.263`, p95 `9.476`, max `23.001`; `loop_slip_ms` p95 `1.739`, max `2.236`, against `poll_tick_ms=250`.

**Implications:**
- Implemented: a small per-tick `RuntimeSnapshotIndex` is built immediately after sampling, with fan pointers keyed by channel and AMD sensor pointers keyed by label. The hot-loop channel helpers now use the index instead of repeatedly calling the linear `FindRuntimeFanChannel` helper.
- This should be treated as a performance cleanup, not an urgent correctness fix: current observed loop timing is comfortably inside the 250 ms budget.

### Q3: Are there data-shape or descriptor opportunities similar to the recent CSV cleanup, but outside runtime CSV rows?

**Answer:** Yes. Offline analyzer CSV parsing still reconstructs repeated field names and map lookups per row. This does not touch live fan behavior, but it is the highest-payoff performance target for large archived CSVs.

**Evidence:**
- `src/analyze/analyze_csv.cpp:27` - `TryParseDouble` creates an owned `std::string` for every numeric parse before calling `std::stod`.
- `src/analyze/analyze_csv.cpp:292` - fan columns are discovered by constructing `"fan" + index + "_"` field names per parsed row.
- `src/analyze/analyze_csv.cpp:318` - channel columns are discovered per parsed row using `TickChannelCsvFieldName`.
- `src/analyze/analyze_channel_sample_columns.cpp:197` - `TickChannelCsvFieldName` allocates a new string for every channel-field lookup.
- `src/analyze/analyze_report_queries.cpp:405` and `src/analyze/analyze_report_queries.cpp:687` - report assembly uses ordered maps and per-channel vectors over DB-loaded samples; this is offline, but can matter on multi-hour captures.

**Implications:**
- Remaining: build a `CsvParsePlan` from the header once, containing column indexes for common fields, fan groups, and channel groups. Then parse rows by index instead of rebuilding field names.
- Implemented: analyzer double parsing now uses `std::from_chars` as a fast path and keeps the previous `std::stod` path as fallback for broader compatibility.
- Keep the analyzer channel descriptor as the source for channel field order; do not add a separate schema engine.

### Q4: Which ideas would change control policy or numerical behavior and therefore need runtime evidence before implementation?

**Answer:** Anything that changes curve shape, source selection, low-band thresholds, cadence behavior, logging cadence, or arithmetic associativity should be evidence-gated. These are not simplification-only changes.

**Evidence:**
- `docs/CONTROL_PIPELINE_MATH.md:21` - computation changes must update the math reference and related runtime docs.
- `docs/CONTROL_PIPELINE_MATH.md:649` - invariants explicitly depend on bounded setpoint, pre-feature equivalence, low-band priority, anti-windup, smoothness, and rate-limit consistency.
- `docs/CONTROL_PIPELINE_MATH.md:779` - real-data passes must verify CSV/status identities before promoting tuning conclusions.
- `docs/COOLING_STRATEGY.md:253` - curve/floor/boost changes require pressure-bias, fan-spacing, radiator-lane, and evidence checks.
- `src/policy/control_policy.cpp:83` - `LookupCurve` linearly scans curve points and computes segment math inline; precomputing inverse spans would be small but can change floating-point rounding/associativity.

**Implications:**
- Do not change `poll_tick_floor_ms`, source-aware guard logic, fan floor/curve points, low-band thresholds, or CSV/status logging cadence as part of a "performance cleanup".
- A curve-evaluation cache or precomputed reciprocal table should be proposed only with exact-output tests and runtime trace comparison, because even tiny numerical differences alter controller setpoints.

### Q5: What tests already cover the risky surfaces, and what tests would be needed before changing them?

**Answer:** Existing coverage is good for the current math split and runtime CSV contracts, but the proposed snapshot-index and analyzer-parse-plan changes need their own focused tests.

**Evidence:**
- `tests/cpp/control_math_tests.cpp:38` - tests smootherstep and shared rate-limit primitives.
- `tests/cpp/boost_stage_tests.cpp:183` - tests boost-stage equivalence against copied legacy helpers.
- `tests/cpp/control_loop_config_tests.cpp:130` - tests boost-stage config loading and validation through `LoadControlLoopConfig`.
- `tests/cpp/csv_rows_tests.cpp:244` - tests control-loop CSV header and representative row values.
- `tests/test_control_loop.py:13` - exercises the staged runtime control loop, writes, boost columns, source-aware paths, low-band, policy gates, and breaker handling.
- `tests/test_analyze_ingest.py:271` - exercises ingest fixtures and DB report inputs through native analyzer paths.

**Implications:**
- Snapshot index change: add a small C++ test around fan-channel lookup/index behavior, then run existing `tests/test_control_loop.py` and local CI.
- Analyzer parse plan change: add a C++ parser test or strengthen `tests/test_analyze_ingest.py` to cover missing/extra repeated columns, then compare ingest row counts on a real archived CSV.
- Math centralization change: extend `control_math_tests` for pressure-scale collapsed-window semantics if that helper is generalized; run boost-stage and core smoke tests.

### Q6: Which docs should hold the resulting queue so it stays separate from historical notes?

**Answer:** Keep this discovery as the new evidence record. `docs/CONTROL_SIMPLIFICATION_TARGETS.md` should remain the completed 2026-05-26 list; `docs/CONTROL_PIPELINE_MATH.md` remains the normative identity reference; `docs/COOLING_STRATEGY.md` remains the policy/tuning gate.

**Evidence:**
- `docs/CONTROL_SIMPLIFICATION_TARGETS.md:3` - the original target list is fully closed.
- `docs/CONTROL_SIMPLIFICATION_TARGETS.md:11` - future behavior-preserving work should use `CONTROL_PIPELINE_MATH.md` as the identity reference.
- `docs/STRUCTURE_AND_STABILITY.md:180` - remaining structural polish is intentionally small and separate from control tuning.

**Implications:**
- Add new implementation items from this discovery only when a specific slice is selected.
- Evidence-gated math/policy proposals should not be merged into the completed simplification record as if they were safe cleanup.

---

## Cross-Cutting Analysis

### Behavior-Preserving Candidates

1. **Runtime snapshot lookup index** - implemented. Removes repeated control-path fan/sensor scans without touching setpoint math.
2. **Centralize remaining smootherstep calls** - implemented with the boost-stage collapsed-window rule preserved.
3. **Fast CSV string escaping/appending** - implemented. `AppendCsvString` no longer allocates an escaped temporary string; emitted bytes remain covered by `csv_rows_tests`.
4. **Analyzer CSV parse plan and double fast path** - partially implemented. The double fast path is in; the header parse plan remains the next larger analyzer refactor.
5. **Report aggregation data-structure cleanup** - lower priority than parser work; useful for large captures after ingest is faster.
6. **Top-level config parser descriptor cleanup** - simplification-only and load-time; do after hotter paths.

### Evidence-Gated Proposals

- Curve point, fan floor, source-aware guard, low-band, or boost-rate changes.
- Adaptive cadence activation or changes to the 250 ms shipped loop cadence.
- Reducing per-tick CSV evidence, changing status schema fields, or altering event emission semantics.
- Curve lookup precomputation that changes floating-point operation order.

### Risks

| Risk | Likelihood | Impact | Notes |
|---|---:|---:|---|
| Snapshot index accidentally observes stale pointers after snapshot mutation | Medium | Medium | Build and consume it within one tick after sampling; do not persist across ticks. |
| Math centralization changes degenerate boost-window behavior | Low | High | Preserve the explicit `full_c <= start_c` full-strength rule and run boost equivalence tests. |
| Analyzer fast parse rejects old-but-valid numeric text | Medium | Medium | Use a tested fallback or scope fast path to runtime-generated CSV. |
| CSV appender changes quoting/precision bytes | Medium | Medium | Compare generated rows by exact string or field-level tests before local CI. |

### Open Questions

- The live runtime timing sample was read from the installed `release\` package, not a freshly published build of the current source tree. It is still useful for priority, but not a source-build benchmark.
- No controlled analyzer benchmark was run during this discovery; parser and report opportunities should be measured against a representative archived CSV before claiming a throughput gain.

---

## Recommendation

The first behavior-preserving slice is complete. The next code-only candidate is optimizing analyzer CSV parsing with a header parse plan and a measured ingest benchmark.

Do not apply math/policy tuning changes until a runtime trace and the checks in `CONTROL_PIPELINE_MATH.md` and `COOLING_STRATEGY.md` justify them.
