# FEAT-0021: Standard control-loop GPU workload context logging

**Project:** svg-mb-control
**Status:** Implemented (T/R verified; live M PASS-with-finding 2026-06-25)   **Version:** 0.3   **Updated:** 2026-06-25
**Namespace:** `REQ-GPUCTX-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/CONTROL_LOOP.md`, `docs/RUNTIME_HOME.md`,
`docs/RUNTIME_LOGGING_AND_EVALUATION.md`,
`docs/as-is-logging-opportunities-2026-06-18.md`,
`docs/logging-next-targets-2026-06-18.md`,
`docs/features/FEAT-0020-standard-control-loop-power-logging.md`
**Purpose:** add a bounded GPU workload context slice to standard control-loop
logs so GPU power and thermal comparisons can be interpreted without changing
control behavior.

## 1. Summary

The standard control-loop CSV logs GPU temperatures and FEAT-0020 GPU board
power. This feature adds the follow-up logging slice for the workload context
behind that power: GPU utilization, pstate, graphics/memory clocks, and VRAM
pressure. The fields are observational only and use the in-repo GPU reader /
evidence source family. The shipped implementation mirrors a cached context
sample into each control-loop row with an explicit sample age instead of adding
a wide read to every 250 ms tick.

## 2. Problem & motivation  *(promotion gate 1)*

The current standard control-loop CSV does not include GPU utilization, clocks,
pstate, or VRAM fields. Those fields are already available in the repo's GPU
evidence contract (`GpuEvidenceSample` and `gpu_evidence_csv.cpp`) and are
written by foreground `evidence-log`, but they are not time-aligned with the
deployed control-loop rows unless an operator also runs a separate evidence
capture.

Power-only comparison can be ambiguous: a GPU power change can come from
different workload intensity, clocks, memory pressure, or pstate rather than a
controller or airflow change. FEAT-0020 should still land narrowly first, but
the likely next target is a small workload-context mirror beside GPU power.

## 3. Goals & non-goals

**Goals**

- Add a small, additive GPU workload context slice to standard control-loop CSV
  rows.
- Reuse the same bounded GPU sample path chosen for FEAT-0020 where possible.
- Preserve sample identity, timestamp, age, and acquisition state so cached or
  unavailable values are explicit.
- Keep old archives ingestible and keep analyzer/reporting header-name based.
- Keep all fields observational and out of fan-control decisions.

**Non-goals**

- No fan duty, curve, cadence, channel, breaker, restore, or write-policy
  change.
- No use of GPU utilization, clocks, pstate, or VRAM as control inputs.
- No sibling repo, HWiNFO, subprocess bridge, or new third-party sensor
  dependency.
- No default-row mirror of wide GPU diagnostics such as throttle reasons, PCIe
  throughput/link state, voltage, GPU fans, power rails, or raw thermal slots.
- No DIMM/RAM/SIO temperature promotion.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| Repo stays standalone | `AGENTS.md` §Repo Boundary | Uses only the existing in-repo GPU reader/evidence machinery; no sibling process or external sensor bridge. |
| Live Runtime Safety | `AGENTS.md` §Live Runtime Safety | The spec is read-only. Runtime restarts or live verification require explicit authorization. |
| Measurement Gate baseline | `docs/MEASUREMENT_GATE.md` | Adds control-loop read/log work only after a bounded cadence decision and runtime cost evidence; no channel/cooldown/cadence policy change is intended. |
| Control-computation identity | `docs/CONTROL_PIPELINE_MATH.md` | GPU workload fields are not inputs to setpoint computation, boost stages, source selection, write gates, or safety overrides. |
| Runtime schema stability | `docs/RUNTIME_HOME.md` | CSV fields are additive and nullable; old archives remain valid and analyzer binds by header name. |

## 5. Behavior specification

The standard control-loop CSV adds a bounded GPU workload-context slice beside
FEAT-0020 GPU board power. The implementation keeps the existing per-tick
`ThermalFast` + board-power read unchanged, then refreshes a cached context
sample at most once per 1000 ms through the in-repo `GpuReader` fast/rare sample
family. Rows between refreshes repeat the same context sample id and expose the
current `gpu_context_sample_age_ms`.

Implemented v1 field set:

- `gpu_context_sample_id`
- `gpu_context_time_ms`
- `gpu_context_sample_age_ms`
- `gpu_context_acquisition`
- `gpu_util_gpu_pct`
- `gpu_util_mem_pct`
- `gpu_pstate`
- `gpu_clock_graphics_mhz`
- `gpu_clock_memory_mhz`
- `gpu_vram_used_mb`
- `gpu_vram_total_mb`

The implementation may rename fields during the required decision record, but
the final set must keep sample identity/time/age and an acquisition marker.
Missing values must be blank/null with an acquisition marker, never false zero.

The context sample identity is separate from FEAT-0020 GPU power because the
power read remains per tick while context is cached. Analyzer/reporting can
compare the values by row timestamp and by the explicit power/context sample
ids and context sample age.

The fields must not feed any control computation. Response-source names remain
the existing temperature/control names and must not gain a GPU-workload-derived
source in this feature.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-GPUCTX-01 | The standard control-loop CSV must include additive GPU workload context fields for utilization, pstate, graphics/memory clocks, VRAM used/total, sample identity, timestamp, sample age, and acquisition marker. |
| REQ-GPUCTX-02 | GPU workload context must come from the existing in-repo GPU reader/evidence source family, preferably the same bounded sample used for FEAT-0020 GPU power logging. |
| REQ-GPUCTX-03 | GPU workload context fields must remain logging-only and must not affect setpoint computation, response-source selection, write gates, safety overrides, breaker behavior, or fan duty. |
| REQ-GPUCTX-04 | The implementation must preserve the shipped cadence profile: added GPU workload reads must keep loop timing and process-resource metrics inside the measurement envelope, or the feature must stay held. |
| REQ-GPUCTX-05 | Analyzer ingest/reporting must bind GPU workload fields by header name, continue to ingest older archives without them, and summarize the context only when present. |
| REQ-GPUCTX-06 | Wide GPU diagnostic fields such as throttle reasons, PCIe, voltage, GPU fans, rails, and raw thermal slots must stay out of this v1 standard-log slice unless this spec is explicitly amended before implementation. |

## 7. Data / schema deltas

- New control-loop CSV fields:
  `gpu_context_sample_id`, `gpu_context_time_ms`,
  `gpu_context_sample_age_ms`, `gpu_context_acquisition`,
  `gpu_util_gpu_pct`, `gpu_util_mem_pct`, `gpu_pstate`,
  `gpu_clock_graphics_mhz`, `gpu_clock_memory_mhz`, `gpu_vram_used_mb`, and
  `gpu_vram_total_mb`.
- Config impact:
  none required for v1 if the fields are enabled only as part of the standard
  GPU power logging profile. If implementation adds a separate config flag,
  update this spec, `README.md`, and runtime docs first.
- Runtime sidecar/status impact:
  none required for v1 unless implementation adds a status mirror. If a status
  mirror is added, update `docs/RUNTIME_HOME.md` and this spec first.
- Schema/version impact:
  additive control-loop CSV schema. Analyzer schema v12 adds nullable
  `tick_samples.gpu_context_*`, `gpu_util_*`, `gpu_pstate`,
  `gpu_clock_*`, and `gpu_vram_*` columns. Older archives without these fields
  ingest/report as unavailable.

## 8. CLI / config / operator surface deltas

No CLI flag is required by this draft. The expected operator surface is the
same standard power-logging profile created for FEAT-0020. If the final design
chooses a separate config knob, the decision record must settle the name,
default, and interaction with FEAT-0020 before implementation.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/logging-next-targets-2026-06-18.md`](../logging-next-targets-2026-06-18.md) (D-GPUCTX-1) | FEAT-0021 remains separate from FEAT-0020; v1 uses the 11-field cached context slice; wide diagnostics stay out of the standard row. | Current |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-GPUCTX-01 | T, R | CSV header/row tests for the context fields; review field names against `docs/RUNTIME_HOME.md` and `docs/RUNTIME_LOGGING_AND_EVALUATION.md`. |
| REQ-GPUCTX-02 | T, R | Unit/integration seam test or review proving the context is sourced from the in-repo GPU reader/evidence path and does not require `evidence-log` foreground mode. |
| REQ-GPUCTX-03 | T, R | Control-loop tests/review showing context fields are not read by setpoint, boost, response-source, write, breaker, or safety paths. |
| REQ-GPUCTX-04 | T, R, M | Local CI and review prove the shipped implementation does not widen the per-tick thermal/power read and refreshes context at a bounded 1000 ms cache cadence; first live deployment should still review achieved interval/slip/overrun/process CPU% against the existing envelope. |
| REQ-GPUCTX-05 | T, R | Analyzer ingest/report tests for archives with and without the new fields; review report output treats context as optional. |
| REQ-GPUCTX-06 | T, R | CSV header tests and review confirm wide diagnostic fields remain out of the v1 standard-log slice. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc, decision record, or source.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Separate FEAT-0021 vs FEAT-0020 revision | implemented | Kept separate; FEAT-0020 remains the power-only feature. |
| Exact field names | implemented | Uses `gpu_context_*` for sample identity/state and short `gpu_*` names for values. |
| Enablement model | implemented | No config knob; context is emitted in the standard control-loop CSV with nullable values. |
| Sample cadence | implemented / live M pending | Per-tick thermal/power read unchanged; context refresh is cached at 1000 ms. Live cadence evidence remains a deployment check. |
| NVML vs non-NVML value preference | implemented | Uses the in-repo GPU reader fast/rare sample family; values stay blank when unavailable. |

## 12. Measurement gate & dependencies

- **Measurement gate:** does not change cadence, write cooldown, live channels,
  or controller strategy. The implementation avoids widening every tick by
  caching context for at least 1000 ms. First live deployment should still review
  runtime cadence/process-resource evidence because it introduces periodic GPU
  context reads.
- **Depends on:** FEAT-0020 GPU power logging decision/cadence work, existing GPU
  evidence reader, analyzer ingest/report support.
- **Build/test impact:** CSV row tests, analyzer ingest/report tests, control
  identity review/tests, runtime verification note, and runtime docs updates at
  implementation. No `docs/CONTROL_PIPELINE_MATH.md` update unless
  implementation changes control identity, which this feature forbids.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, Measurement Gate, control identity, and runtime schema stability (§4).
- [x] 3. Required design decision record(s) written and marked current (§9; D-GPUCTX-1 is Current).
- [x] 4. Concrete `REQ-GPUCTX-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks and mirrored in `docs/TRACEABILITY.md` (§10).
- [x] 6. Confirmed it does not violate Live Runtime Safety or Repo Boundary, and does not silently move the Measurement Gate baseline; live cadence evidence remains tracked under REQ-GPUCTX-04 before/at deployment (§12).
- [x] 7. Doctrine check: claims are grounded; proposed behavior is labeled proposed; no undefined vague terms.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-GPUCTX-01 | pass | T+R. `runtime_csv_rows.cpp` emits the 11 additive context fields; `csv_rows_tests` locks header/row alignment and values; `test_control_loop.py::test_control_loop_logs_gpu_board_power` verifies simulated live rows include context fields. `Test-LocalCI.ps1 -KeepBuildDir` passed. | 2026-06-20 |
| REQ-GPUCTX-02 | pass | T+R. `GpuReader::Sample()` owns the context cache and sources values from the in-repo GPU reader fast/rare sample family; simulation uses the same in-repo reader seam. No `evidence-log` foreground process, sibling repo, or subprocess bridge is required. | 2026-06-20 |
| REQ-GPUCTX-03 | pass | T+R. Context fields are copied only into `RuntimeGpuSnapshot` and the control CSV/analyzer; `channel_evaluator` still consumes only GPU temperatures via `GpuControlEnvelopeC`. `test_control_loop.py` keeps `channel0_response_source=primary_curve` with GPU power/context present. | 2026-06-20 |
| REQ-GPUCTX-04 | pass (with finding) | T+R+M. T/R: per-tick thermal/power sample unchanged, context refreshed at most once per 1000 ms; full `Test-LocalCI.ps1 -KeepBuildDir` passed. M (live, clean-tree build 2026-06-23): PRIMARY `archive/svg_mb_control_control-loop_20260624_180903.csv` (57363 rows) holds the section-10-named envelope — `loop_achieved_interval_ms` p99 251.97 ms (baseline 251.92), `loop_slip_ms` p99 1.97 ms, `loop_overrun` frac 7e-05 (baseline 0), `process_cpu_pct` p99 0.156 % (baseline 0.146). Context read isolated by `gpu_context_sample_age_ms` band: refresh-tick `loop_work_duration_ms` p50 42.71 / p99-bulk 61.40 ms vs cached p50 1.615 ms == pre-FEAT-0021 baseline 1.582 ms — the ~41 ms read fires once per ~1000 ms, inside the 250 ms budget. Finding (non-breaching): refresh-tick overrun ~3–4× cached, stalls concentrate on the `age==0` read tick (LIVE P<0.0001), largest stalls on cached no-read ticks (pre-existing FEAT-0020 environmental class). `docs/feat-0021-live-cadence-evidence-2026-06-25.md`. | 2026-06-25 |
| REQ-GPUCTX-05 | pass | T+R. Analyzer schema v12 adds nullable context columns; `test_analyze_ingest.py::test_report_derives_gpu_context_distribution` verifies ingest/report summary, `...old archive...` coverage verifies missing fields report unavailable, and the v9 migration test now migrates through v12. | 2026-06-20 |
| REQ-GPUCTX-06 | pass | T+R. CSV header tests and review confirm v1 standard rows include only utilization, pstate, graphics/memory clocks, VRAM used/total, identity/time/age/acquisition. Wide fields such as throttle reasons, PCIe, voltage, GPU fans, power rails, and raw thermal slots remain out of the standard row. | 2026-06-20 |

**Spec vs. implementation deltas:** The final design does not reuse the exact
per-tick FEAT-0020 power sample for context. It keeps FEAT-0020 power per tick
and adds a separate cached FEAT-0021 context sample with a 1000 ms minimum
refresh interval and explicit `gpu_context_sample_age_ms`. `REQ-GPUCTX-04`
was `partial` until the live cadence M ran 2026-06-25 and is now `pass (with
finding)`: the section-10-named envelope holds (achieved-interval p99 251.97 ms,
slip p99 1.97 ms, overrun frac 7e-05, `process_cpu_pct` p99 0.156 %) and the
~41 ms context read fires once per ~1000 ms inside the 250 ms budget; the
non-breaching finding (refresh-tick overrun ~3–4× cached; multi-second stalls
concentrate on the `age==0` read tick, LIVE P<0.0001) is recorded in
`docs/feat-0021-live-cadence-evidence-2026-06-25.md`.
