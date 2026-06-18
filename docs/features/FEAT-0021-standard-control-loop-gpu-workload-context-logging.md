# FEAT-0021: Standard control-loop GPU workload context logging

**Project:** svg-mb-control
**Status:** Draft   **Version:** 0.1   **Updated:** 2026-06-18
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

The standard control-loop CSV currently logs GPU temperatures and, once
FEAT-0020 is implemented, is expected to log GPU power. This feature captures a
follow-up logging slice for the workload context behind that power: GPU
utilization, pstate, graphics/memory clocks, and VRAM pressure. The fields are
observational only and must use the same in-repo GPU evidence source family as
foreground `evidence-log`.

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

The standard control-loop CSV must add a bounded GPU workload-context slice only
after FEAT-0020's GPU power sample path has a settled cadence/read-cost decision
or this feature records its own equivalent decision.

Preferred v1 field set:

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

If the implementation reuses the same sample as FEAT-0020 GPU power, the sample
identity may be shared or duplicated by name, but analyzer/reporting must be
able to prove which power and context values came from the same GPU sample.

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

- Proposed new control-loop CSV fields:
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
  additive control-loop CSV schema. Analyzer code must handle missing fields in
  older archives.

## 8. CLI / config / operator surface deltas

No CLI flag is required by this draft. The expected operator surface is the
same standard power-logging profile created for FEAT-0020. If the final design
chooses a separate config knob, the decision record must settle the name,
default, and interaction with FEAT-0020 before implementation.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/logging-next-targets-2026-06-18.md`](../logging-next-targets-2026-06-18.md) (D-GPUCTX-1) | Whether GPU workload context follows FEAT-0020 as a separate feature, the minimal field set, and the rule that wide diagnostics stay in `evidence-log` by default. | Proposed |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-GPUCTX-01 | T, R | CSV header/row tests for the context fields; review field names against `docs/RUNTIME_HOME.md` and `docs/RUNTIME_LOGGING_AND_EVALUATION.md`. |
| REQ-GPUCTX-02 | T, R | Unit/integration seam test or review proving the context is sourced from the in-repo GPU reader/evidence path and does not require `evidence-log` foreground mode. |
| REQ-GPUCTX-03 | T, R | Control-loop tests/review showing context fields are not read by setpoint, boost, response-source, write, breaker, or safety paths. |
| REQ-GPUCTX-04 | M, R | Runtime evidence from the standard 250 ms profile: achieved interval, slip/overrun, process CPU%, and health stay inside the current measurement envelope. |
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
| Separate FEAT-0021 vs FEAT-0020 revision | implementation | Keep separate so FEAT-0020 can land narrowly. |
| Exact field names | implementation | Use `gpu_context_*` for sample identity/state and short `gpu_*` names for values. |
| Enablement model | implementation | Enable only with the standard power-logging profile unless a separate config knob proves necessary. |
| Sample cadence | implementation/runtime evidence | Reuse FEAT-0020's bounded GPU sample cadence if available; otherwise prove this feature's cadence separately. |
| NVML vs non-NVML value preference | implementation | Prefer available NVML utilization/clocks where the existing evidence sample exposes them, with blanks when unavailable. |

## 12. Measurement gate & dependencies

- **Measurement gate:** does not intentionally change cadence, write cooldown,
  live channels, or controller strategy, but it adds read/log work to the
  control loop. Promotion requires runtime cadence/process-resource evidence or
  a proven shared sample path from FEAT-0020.
- **Depends on:** FEAT-0020 GPU power logging decision/cadence work, existing GPU
  evidence reader, analyzer ingest/report support.
- **Build/test impact:** CSV row tests, analyzer ingest/report tests, control
  identity review/tests, runtime verification note, and runtime docs updates at
  implementation. No `docs/CONTROL_PIPELINE_MATH.md` update unless
  implementation changes control identity, which this feature forbids.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, Measurement Gate, control identity, and runtime schema stability (§4).
- [ ] 3. Required design decision record(s) written and marked current (§9; D-GPUCTX-1 is Proposed, not Current).
- [x] 4. Concrete `REQ-GPUCTX-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks and mirrored in `docs/TRACEABILITY.md` (§10).
- [ ] 6. Confirmed it does not violate Live Runtime Safety or Repo Boundary, and does not silently move the Measurement Gate baseline; runtime cadence evidence is still pending (§12).
- [x] 7. Doctrine check: claims are grounded; proposed behavior is labeled proposed; no undefined vague terms.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-GPUCTX-01 | | | |
| REQ-GPUCTX-02 | | | |
| REQ-GPUCTX-03 | | | |
| REQ-GPUCTX-04 | | | |
| REQ-GPUCTX-05 | | | |
| REQ-GPUCTX-06 | | | |

**Spec vs. implementation deltas:** <record at implementation.>
