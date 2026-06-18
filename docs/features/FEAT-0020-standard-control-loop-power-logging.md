# FEAT-0020: Standard control-loop power logging

**Project:** svg-mb-control
**Status:** Implemented (T/B/R verified 2026-06-18; live-flip M-evidence deferred)   **Version:** 0.3   **Updated:** 2026-06-18
**Namespace:** `REQ-PWRLOG-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/CONTROL_LOOP.md`, `docs/RUNTIME_HOME.md`,
`docs/RUNTIME_LOGGING_AND_EVALUATION.md`,
`docs/features/FEAT-0006-cpu-work-energy-efficiency-evidence.md`
**Purpose:** make CPU package power and GPU power visible in the standard
control-loop logs for comparison, without changing control behavior.

## 1. Summary

The standard control loop already logs CPU temperature, GPU temperature, fan
state, loop timing, and the FEAT-0006 CPU energy columns. In the current live
runtime, the CPU energy columns remain disabled by environment, and GPU power is
available only in foreground `evidence-log` CSVs. This feature plans the flip to
record comparable CPU package power and GPU power in the same standard
control-loop CSV while keeping the fan-control law temperature-driven.

## 2. Problem & motivation  *(promotion gate 1)*

Observed runtime evidence on 2026-06-18 showed `control_runtime.json` healthy and
standard control-loop CSV rows active, but the last 1200 sampled rows had
`cpu_pkg_energy_acquisition=disabled`, `cpu_cycles_acquisition=disabled`, no CPU
energy sample ids, and no GPU power columns. The current `release` environment
had `SVG_MB_CONTROL_RAPL_ENERGY_MODE=disabled` and
`SVG_MB_CONTROL_CPU_CYCLES_MODE=disabled`, and the latest
`scripts/Reset-EnergyToDisabled.ps1` safety-revert log confirmed those markers
were held disabled immediately before the current worker started.

The code/contract gap is split:

- CPU package energy already has an accepted read-only acquisition feature
  (`FEAT-0006`) and existing control-loop CSV columns, but the standard runtime
  profile does not enable the acquisition gate.
- GPU power has an in-repo evidence source (`GpuReader::SampleEvidence` and
  `gpu_evidence_nvml_power_mw` in foreground `evidence-log`) but the standard
  control-loop CSV only logs GPU temperatures.

For tuning and comparison, CPU package power and GPU power need to be visible in
the same standard runtime log window. Running a separate `evidence-log` is useful
for diagnostics, but it is not the same time-aligned stream as the deployed
control loop.

## 3. Goals & non-goals

**Goals**

- Enable existing CPU package-energy logging in the standard control-loop
  runtime through an explicit operator/deployment profile.
- Add additive GPU power fields to the standard control-loop CSV.
- Preserve field provenance so acquisition state is distinguishable and no false
  zero is emitted. The CPU live marker (`cpu_pkg_energy_acquisition`) is produced
  by the worker as one of `{disabled, unavailable, quarantine}` only; `validated`
  is a separate post-capture script promotion, never emitted by the live loop.
- Keep all power data observational; analyzer/reporting derives summaries and
  comparisons later.
- Prove that the logging flip does not change control decisions or the shipped
  250 ms cadence profile.

**Non-goals**

- No fan duty, curve, channel, cadence, breaker, restore, or write-policy change.
- No use of CPU or GPU watts as a control input.
- No promotion of FEAT-0006 `quarantine` markers to `validated`; that remains a
  separate maintainer decision.
- No sibling repo, HWiNFO, subprocess bridge, or third-party sensor dependency.
- No CPU cycle logging unless a comparison run explicitly asks for
  effective-frequency context.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| Repo stays standalone | `AGENTS.md` §Repo Boundary | Uses the existing in-repo AMD/PawnIO and GPU reader paths; no sibling repos or external sensor subprocesses. |
| Live Runtime Safety | `AGENTS.md` §Live Runtime Safety | The spec is read-only. Any worker restart or scheduled-task change for the operational flip requires explicit live-runtime authorization. |
| Measurement Gate baseline | `docs/MEASUREMENT_GATE.md` | Does not change tick cadence, write cooldown, live channels, or controller strategy. Runtime evidence must show the added reads do not degrade the shipped 250 ms profile. |
| Control-computation identity | `docs/CONTROL_PIPELINE_MATH.md` | Power fields are not inputs to setpoint computation, boost stages, write gates, or safety overrides. |
| Runtime schema stability | `docs/RUNTIME_HOME.md` | New CSV fields are additive and nullable; old archives must continue to ingest by header name. |
| FEAT-0006 marker semantics | `docs/features/FEAT-0006-cpu-work-energy-efficiency-evidence.md` | Standard logging may enable the existing acquisition path, but does not change `quarantine`/`validated` promotion rules. |

## 5. Behavior specification

### CPU package power

The standard control-loop worker must be started with
`SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled` when the operator selects the power
logging profile. This uses the existing FEAT-0006 fields:

- `cpu_power_sample_id`
- `cpu_power_window_ms`
- `cpu_pkg_energy_delta_uj`
- `cpu_pkg_energy_acquisition`

Average CPU package watts remain derived by analyzer/reporting as
`(cpu_pkg_energy_delta_uj / 1e6) / (cpu_power_window_ms / 1000)` over distinct
nonzero `cpu_power_sample_id` values. The logger must not add a second CPU watts
column that could drift from that derivation.

`SVG_MB_CONTROL_CPU_CYCLES_MODE` remains disabled by the default power-logging
profile. It can be enabled only for runs that explicitly need APERF/MPERF
effective-frequency context.

### GPU power

The standard control-loop CSV must add a bounded, read-only GPU power slice. The
field set is (decided 2026-06-18, D-PWRLOG-1):

- `gpu_power_sample_id` — a GPU-power read counter that advances only when a fresh
  board-power read succeeds; it does not advance on a skipped or failed read, so a
  repeated id makes a stale/mirrored value explicit.
- `gpu_power_time_ms` — the timestamp of the GPU power read itself (the sampler's
  per-read clock), not the control-tick time; this carries a read-latency/staleness
  signal for the REQ-PWRLOG-04 cadence evidence.
- `gpu_power_mw` — instantaneous board-total milliwatts; blank when unavailable.
- `gpu_power_source` — `unknown` unless the NVML read returns nonzero, then `nvml`.
- `gpu_power_acquisition` — `disabled` / `unavailable` / `nvml` marker.

The implementation reads GPU power per control tick by adding a single board-power
read to the existing per-tick GPU thermal sample (no separate evidence-log pass).
The five fields form a **cadence-agnostic** schema: under the per-tick read the
sample id/timestamp track each tick, and the same fields remain correct if a future
bounded cached cadence is adopted (the analyzer dedups by `gpu_power_sample_id`),
so no schema-version redo is needed if the cadence decision changes. Missing GPU
power must emit blank numeric fields plus `gpu_power_acquisition=unavailable` or
`disabled`, not zero.

The GPU power source must come from the same in-repo GPU evidence machinery used
by `evidence-log`, or a narrower helper around that same backend. Full
`evidence-log` mode must not be required for standard runtime comparison.

### Control behavior

Power fields must not feed any control computation. The setpoint path must
continue to use the existing temperature/blend/boost inputs. The log may show
`primary_curve`, `cpu_override`, `midband_pressure`, `gpu_airflow`, and
`cpu_low_soak` response sources exactly as before; it must not introduce a
power-derived response source in this feature.

### Operator flip

The implementation must document a reversible operator workflow:

1. enable the standard CPU package-energy logging profile,
2. make sure the safety-revert task cannot silently undo that profile,
3. restart the worker only under explicit live-runtime authorization,
4. verify standard control-loop rows contain CPU and GPU power evidence, and
5. revert to the disabled profile if needed.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-PWRLOG-01 | The standard control-loop logging profile must enable FEAT-0006 CPU package-energy acquisition by starting the worker with `SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled`, while preserving the existing `cpu_pkg_energy_acquisition` marker semantics. |
| REQ-PWRLOG-02 | The standard control-loop CSV must include additive GPU power fields with sample identity, timestamp, source, acquisition marker, and nullable/blank numeric values when unavailable. |
| REQ-PWRLOG-03 | CPU package power and GPU power must remain logging-only signals and must not affect setpoint computation, response-source selection, write gates, safety overrides, breaker behavior, or fan duty. |
| REQ-PWRLOG-04 | The power logging flip must preserve the shipped cadence profile: under the standard 250 ms control loop, added power reads must keep loop timing and process-resource metrics inside the current measurement-gate envelope, or the feature must stay held. |
| REQ-PWRLOG-05 | Analyzer ingest/reporting must bind the new fields by header name, continue to ingest older archives without GPU power fields, derive CPU package watts only from existing FEAT-0006 energy/window columns, and summarize GPU power only when present. |
| REQ-PWRLOG-06 | The operator flip/revert workflow must be explicit and documented, including the interaction with `scripts/Reset-EnergyToDisabled.ps1`, the `SVG-MB Energy Safety Revert` task, and the live-runtime restart requirement. |

## 7. Data / schema deltas

- New control-loop CSV fields:
  `gpu_power_sample_id`, `gpu_power_time_ms`, `gpu_power_mw`,
  `gpu_power_source`, and `gpu_power_acquisition`.
- Existing FEAT-0006 CPU fields remain unchanged:
  `cpu_power_sample_id`, `cpu_power_window_ms`,
  `cpu_pkg_energy_delta_uj`, and `cpu_pkg_energy_acquisition`.
- Config/operator impact:
  an explicit standard logging profile must set
  `SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled` for the worker. The spec does not
  require enabling CPU cycles.
- Runtime sidecar/status impact:
  none required for v1 unless implementation adds a status mirror. If a status
  mirror is added, update `docs/RUNTIME_HOME.md` and this spec first.
- Schema/version impact:
  control-loop CSV schema is additive. Analyzer code must handle missing GPU
  power fields in older archives.

## 8. CLI / config / operator surface deltas

The implementation must update `README.md` with the standard power-logging
profile and the safe verification commands. If the workflow uses a script or
scheduled-task update, it must stay in this repo and use the documented build and
runtime entrypoints.

No CLI flag is required by this spec. A CLI/config flag may be added if the
implementation chooses a repo-owned alternative to persistent environment
variables, but that choice must be settled in the decision record before code
work starts.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/power-logging-flip-plan-2026-06-18.md`](../power-logging-flip-plan-2026-06-18.md) (D-PWRLOG-1) | The standard flip direction: enable existing CPU RAPL package-energy logging for comparison, add the cadence-agnostic 5-field GPU power slice (per-tick read, read-timestamp) to the standard control-loop CSV, summarize GPU power as mean/percentile (not the CPU energy integral), keep control math unchanged, and use a reversible operator profile that disables the safety-revert task. | Current |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-PWRLOG-01 | T, M, R | Config/script or startup test for exact CPU RAPL env gate; runtime CSV evidence with nonempty CPU energy sample ids; review vs FEAT-0006 marker semantics. |
| REQ-PWRLOG-02 | T, M, R | CSV header/row test for GPU power fields; runtime CSV evidence with nonempty GPU power samples; review no false-zero behavior. |
| REQ-PWRLOG-03 | T, R | Control-loop tests/review showing power fields are not read by setpoint/boost/write code; review `docs/CONTROL_PIPELINE_MATH.md` needs no identity change. |
| REQ-PWRLOG-04 | M, R | Runtime evidence from the standard 250 ms profile: achieved interval, slip/overrun, process CPU%, and health remain within the current measurement-gate envelope; review GPU read cadence. |
| REQ-PWRLOG-05 | T, R | Analyzer ingest/report tests for new fields and old archives; review CPU watts derivation remains FEAT-0006-derived, not double-logged. |
| REQ-PWRLOG-06 | T, M, R | Script/workflow test or dry-run review for flip/revert; live verification only after explicit authorization; README/operator docs review. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc, decision record, or source.

## 11. Open decisions

| Decision | Needed before | Resolution (2026-06-18, D-PWRLOG-1) |
|---|---|---|
| GPU power read cadence | implementation | **Resolved: per-tick read** (one `nvmlDeviceGetPowerUsage` added to the existing per-tick GPU thermal sample), with a **cadence-agnostic 5-field schema** so a later cached cadence needs no schema redo. |
| GPU power schema field set | implementation | **Resolved: 5 fields** with the read-timestamp upgrade (`gpu_power_time_ms` = NVML read clock; `gpu_power_sample_id` = read counter that does not advance on a skipped/failed read), satisfying REQ-PWRLOG-02 as written. |
| Operator flip mechanism | implementation | **Resolved: a reversible profile pair** (`scripts/Set-EnergyLoggingProfile.ps1 -Enable/-Disable`) that disables/re-enables the `SVG-MB Energy Safety Revert` task and sets the worker env, then restarts via the documented path. |
| Whether to enable CPU cycles with power logging | implementation/runtime run | Keep disabled by default; enable only for runs needing effective-frequency context. |
| Whether to mirror GPU power in `current_state.json` | implementation | Do not mirror in v1; CSV comparison is the requested surface. |

## 12. Measurement gate & dependencies

- **Measurement gate:** does not change cadence, write cooldown, live channels,
  or controller strategy. It does add read/log work to the control loop, so
  promotion requires runtime cadence/process-resource evidence before the flip
  becomes standard.
- **Depends on:** FEAT-0006 CPU energy acquisition; existing GPU evidence reader;
  analyzer ingest/report support.
- **Build/test impact:** CSV row tests, analyzer ingest/report tests, config or
  script workflow tests, and a runtime verification note. No
  `docs/CONTROL_PIPELINE_MATH.md` update unless implementation accidentally
  changes control identity, which this feature forbids.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence and a named code/contract gap (§2).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, Measurement Gate, control identity, runtime schema stability, and FEAT-0006 marker semantics (§4).
- [x] 3. Required design decision record(s) written and marked current (§9; D-PWRLOG-1 promoted to Current 2026-06-18).
- [x] 4. Concrete `REQ-PWRLOG-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks and mirrored in `docs/TRACEABILITY.md` (§10).
- [x] 6. Measurement-gate validation method defined (the runtime evidence itself is deferred to §14 / REQ-PWRLOG-04, which is `M` and can only close after a live flip): REQ-PWRLOG-04 acceptance is a same-machine/same-build pre-flip 250 ms baseline capture compared to a post-flip capture (achieved interval, `loop_work_duration_ms`, slip/overrun, process CPU%, health). The added per-tick GPU read is the only new hot-path cost; the flip needs separate live-runtime authorization (§4/§5). Buildable work does not move the live baseline because the flip is a separate authorized step.
- [x] 7. Doctrine check: claims are grounded in current runtime/source evidence; proposed behavior is labeled proposed; no undefined vague terms.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-PWRLOG-01 | partial | T+R done; live M deferred. T: `tests/test_energy_logging_profile.py` (`-Enable` dry-run sets `SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled` and disables the safety-revert task). R: marker semantics preserved — the worker emits `{disabled, unavailable, quarantine}` only, never live `validated` (`amd_reader.cpp`). M: live CSV with nonempty CPU energy sample ids deferred to the authorized flip window. | 2026-06-18 |
| REQ-PWRLOG-02 | partial | T+R done; live M deferred. T: `csv_rows_tests` (5 GPU columns, header/row aligned), `test_control_loop.py::test_control_loop_logs_gpu_board_power` + `…gpu_power_unavailable_emits_no_false_zero`, `test_analyze_ingest.py::test_report_derives_gpu_power_distribution`. R: blank-plus-marker (no false zero) reviewed. M: live nonempty GPU power samples deferred to the flip window. | 2026-06-18 |
| REQ-PWRLOG-03 | pass | T: `test_control_loop.py` asserts `channel0_response_source` stays `primary_curve` with GPU power present. R: power is not in `TempInputs`/`EvaluateChannel`; `power_anticipation.h` stays unreferenced by `src/control`/`src/runtime`; `docs/CONTROL_PIPELINE_MATH.md` unchanged. Full Test-LocalCI green (CTest + 169 hermetic). | 2026-06-18 |
| REQ-PWRLOG-04 | partial | R done; runtime M deferred (gate 6). R: GPU power is one `nvmlDeviceGetPowerUsage` piggybacked on the existing per-tick thermal sample (`gpu_probe.cpp` `poll_nvml_board_power`); CPU energy read stays off the PCI mutex. M: same-machine/same-build pre/post-flip 250 ms cadence evidence (`loop_work_duration_ms`/slip/overrun/`process_cpu_pct`/health) deferred to the authorized live-flip window. | 2026-06-18 |
| REQ-PWRLOG-05 | pass | T: `test_analyze_ingest.py::test_report_derives_gpu_power_distribution` (mean 233333.33 over 3 de-duplicated samples — not an energy integral), the old-archive degrade case (sample_count 0, `unavailable`), and `test_ingest_migrates_v9_db_to_v10` (ladder to v11). R: CPU watts derivation unchanged, no second CPU watts column; v10→v11 additive positional ingest. | 2026-06-18 |
| REQ-PWRLOG-06 | partial | T+R done; live M deferred. T: `tests/test_energy_logging_profile.py` (`-Enable`/`-Disable` dry-run toggles the `SVG-MB Energy Safety Revert` task + the User env; requires exactly one mode). R: reversible profile coexists with the safety revert; the FEAT-0006 boot-OFF-guarantee inversion is recorded in D-PWRLOG-1. M: live flip/revert verification deferred to the authorized window. | 2026-06-18 |

**Spec vs. implementation deltas:** GPU power read is a single `nvmlDeviceGetPowerUsage` added to the vendored `sample_thermal_fast` via `poll_nvml_board_power` (board power only, not the per-rail topology); read-timestamp is a reader-owned monotonic-ms clock (`time_ms` is caller-owned, not stamped by the sampler). Analyzer summary is mean/p50/p90/max over distinct `gpu_power_sample_id` samples (schema v11), explicitly not the CPU Sigma-energy integral. CPU side (Track A) added no worker code. Live-flip M evidence (REQ-PWRLOG-04 and the live parts of -01/-02/-06) is deferred to a separately authorized live-runtime window.
