# FEAT-0018: Adaptive-cadence enablement under thermal transient

**Project:** svg-mb-control
**Status:** Draft   **Version:** 0.1   **Updated:** 2026-06-18
**Namespace:** `REQ-CADENCE-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/CONTROL_LOOP.md`, `docs/MEASUREMENT_GATE.md`,
`docs/RUNTIME_LOGGING_AND_EVALUATION.md`
**Purpose:** enable the already-implemented adaptive-cadence engine in the shipped
config so the control loop shortens its tick toward a characterized floor during a
thermal transient and relaxes back to `250 ms` when slew subsides, halving
detection latency under load without raising steady-state cost beyond the
measurement-gate bound.

> Draft / design capture. This is not implementation authorization. The direction
> is recorded in `docs/control-latency-reduction-design-2026-06-18.md`
> (D-CADENCE-1, Proposed). It **crosses `docs/MEASUREMENT_GATE.md`**; promotion
> requires the characterization evidence in §12.

## 1. Summary

The adaptive-cadence engine exists in code (`src/control/cadence_score.cpp`
`ComputeCadence`) and is fully described in `docs/CONTROL_LOOP.md` (Inputs:
`poll_tick_floor_ms`, `cadence_slew_start_c_per_s`, `cadence_slew_full_c_per_s`,
`cadence_relax_per_s`). In the shipped `release/control.json` it is **inert**:
`poll_tick_floor_ms` is absent, so `F >= P` short-circuits `ComputeCadence` and the
loop runs at a fixed `250 ms`. This feature enables the floor with characterized
values so that, when CPU/GPU temperature slew rises, the loop tightens its tick
toward the floor (instant tighten, rate-limited relax), reducing the
`<= poll_tick_ms` detection delay during the transient where reaction matters most.
The mechanism is unchanged; only its config inputs and the supporting measurement
evidence are new.

## 2. Problem & motivation  *(promotion gate 1)*

Named gap, grounded in source and shipped config:

1. **The latency-reduction engine is shipped-off.** `ComputeCadence` returns
   `effective_interval_ms = poll_tick_ms` and no transient whenever `F >= P`
   (`src/control/cadence_score.cpp`, the `if (F >= P)` short-circuit). The shipped
   `control_loop` block (`release/control.json`) has no `poll_tick_floor_ms`, so the
   default-inert path is always taken and the loop is byte-identical to a fixed
   `250 ms` tick.
2. **Detection latency is fixed at one tick.** Each tick samples in-process
   (`src/control/tick_runner.cpp` `RunControlTick`), so a temperature change is seen
   at most `poll_tick_ms` later (`docs/control-latency-reduction-design-2026-06-18.md`
   §2, Detect term). With adaptation off, that `<= 250 ms` delay applies equally
   during a fast transient and at idle, even though the engine was built to shorten
   it precisely during transients.
3. **The design intent is unrealized.** `docs/CONTROL_LOOP.md` and
   `docs/adaptive-cadence-design-2026-05-19.md` describe the slew-driven floor as the
   intended behavior; it has never been engaged in a shipped profile because
   enabling a floor below `250 ms` is blocked by the measurement gate until
   characterized.

The fix is config plus evidence: set a floor and slew thresholds, and produce the
`MEASUREMENT_GATE.md` characterization that the gate requires before any floor below
the shipped profile is adopted.

## 3. Goals & non-goals

**Goals**
- Engage `ComputeCadence` in the shipped config by setting `poll_tick_floor_ms`
  below `poll_tick_ms`, with `cadence_slew_start_c_per_s`,
  `cadence_slew_full_c_per_s`, and `cadence_relax_per_s` tuned so tightening fires
  on a real thermal transient and relaxes back to `poll_tick_ms` when slew subsides.
- Reduce detection latency during transients while keeping steady-state achieved
  interval, `loop_slip_ms`, `loop_overrun`, and process CPU% within the
  measurement-gate exit criteria.
- Produce and record the `MEASUREMENT_GATE.md` characterization evidence that
  authorizes a floor below the shipped profile.

**Non-goals**
- No change to `poll_tick_ms` itself (the idle/relaxed interval stays `250 ms`) and
  no change to `write_cooldown_ms` (write spacing is unchanged; this feature affects
  *sampling/decision* cadence, not write frequency).
- No change to curves, channel set, mixed-input strategy, boost overlays, or the
  control-computation identity.
- No new control law; `ComputeCadence` and its rate-limited relax are used as-is.
- No change to the `loop_slip_ms` / `loop_overrun` definitions, which stay relative
  to `poll_tick_ms` (`docs/CONTROL_LOOP.md` Outputs).

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| Adaptive-cadence floors below the shipped profile are blocked until characterized | `docs/MEASUREMENT_GATE.md` (What This Blocks) | This is the central stressed invariant. The feature does not adopt a floor until the gate's Required Steps and Exit Criteria are met and a compact summary is recorded (REQ-CADENCE-05, §12). |
| Shipped `250 ms` write cooldown / channel set is the measured baseline | `docs/MEASUREMENT_GATE.md` | `write_cooldown_ms` and channel membership are unchanged; only the sampling/decision floor moves, and only after characterization. |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | `EvaluateChannel` is unchanged. Per-tick elapsed inputs already vary with achieved interval; the smoothing/rate math is the same. No identity term is added. |
| Runtime CSV / status / manifest schema stays backward-compatible | `docs/RUNTIME_HOME.md` | No new field. `cadence_transient` and the loop-timing fields already exist in the CSV (`docs/CONTROL_LOOP.md` Outputs); enabling the floor only makes them vary. |
| No fan write / authority change outside an explicit live task | `AGENTS.md` §Live Runtime Safety | Characterization captures are explicit live runs; the feature changes sampling cadence, not the write-authority model. |

## 5. Behavior specification

The change is config-only in the `control_loop` block of
`config/control.release.json`, consumed by `src/control/cadence_score.cpp`
(`ComputeCadence`) and `src/control/tick_runner.cpp` (which calls it and passes
`cadence.effective_interval_ms` to `WaitForNextControlTick`). The validation fixture
under `tests/` is the only code touched.

- **Floor engaged.** `poll_tick_floor_ms` is set to a characterized value with
  `25 <= poll_tick_floor_ms < poll_tick_ms` (`docs/CONTROL_LOOP.md` Inputs), so
  `ComputeCadence` takes its active path instead of the `F >= P` short-circuit.
- **Slew-driven tighten.** As `max(CPU, GPU)` temperature slew (per
  `cadence_score.cpp`, `SmoothScale` between `cadence_slew_start_c_per_s` and
  `cadence_slew_full_c_per_s`) rises, the effective interval shortens toward the
  floor; tighten is immediate.
- **Rate-limited relax.** When slew subsides, the effective interval relaxes back
  toward `poll_tick_ms` at `cadence_relax_per_s` (default
  `(poll_tick_ms - poll_tick_floor_ms) / 3` if unset), so the loop does not chatter
  between fast and slow ticks.
- **Idle unchanged.** With no transient (`transient = 0`), the effective interval is
  `poll_tick_ms` — idle behavior, CPU cost, and write spacing are the shipped
  behavior.
- **Write cadence unchanged.** `write_cooldown_ms` still gates writes, so a faster
  sampling floor does not increase write frequency beyond what deadband + cooldown
  already bound.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-CADENCE-01 | The shipped `control_loop` config must set `poll_tick_floor_ms` to a characterized value with `25 <= poll_tick_floor_ms < poll_tick_ms` so `ComputeCadence` engages its active path; with the floor set, a sustained CPU/GPU temperature slew at or above `cadence_slew_full_c_per_s` must drive the effective interval to `poll_tick_floor_ms`. |
| REQ-CADENCE-02 | `cadence_slew_start_c_per_s` and `cadence_slew_full_c_per_s` (with `full > start`) and `cadence_relax_per_s` must be set so tightening does not fire below the configured start slew and the effective interval relaxes back to `poll_tick_ms` after a transient (no permanent fast tick at steady state). |
| REQ-CADENCE-03 | Under the enabled floor, steady-state achieved interval, `loop_slip_ms`, `loop_overrun` (which stays defined relative to `poll_tick_ms`, `tick_runner.cpp`), and process CPU% must stay within the `MEASUREMENT_GATE.md` exit criteria, demonstrated by the characterization capture; and while the effective interval is tightened toward the floor under a sustained full-slew transient, per-tick work duration must not exceed the active (floor) interval. |
| REQ-CADENCE-04 | `write_cooldown_ms` and channel membership must be unchanged, so the faster floor reduces detection/decision latency only and does not increase write frequency or actuation spam beyond the existing deadband + cooldown bound. |
| REQ-CADENCE-05 | A `MEASUREMENT_GATE.md` characterization (AMD/SIO/GPU input cadence and fan-write response at the chosen floor) must exist and be summarized per the gate's Required Steps and Exit Criteria before the floor is adopted; the floor value and slew thresholds must be justified by that evidence. |

## 7. Data / schema deltas

- New/changed fields: none in runtime-home/CSV/status. The config gains
  `poll_tick_floor_ms`, `cadence_slew_start_c_per_s`, `cadence_slew_full_c_per_s`,
  and `cadence_relax_per_s` in the `control_loop` block — all already defined and
  parsed (`docs/CONTROL_LOOP.md` Inputs); absent today, present after this feature.
- Config impact (`config/control.*.json`): the `control_loop` block gains the four
  optional cadence keys with characterized values.
- Schema/version impact: none. `cadence_transient` and the loop-timing CSV/status
  fields already exist; no `schema_version` bump. Existing archives stay valid (the
  fields read as the fixed-cadence values for pre-feature runs).

## 8. CLI / config / operator surface deltas

- No new CLI subcommand or flag. The four cadence keys are existing optional config
  fields.
- `--status` already surfaces the loop-timing fields; no new status field. UI is out
  of scope (`docs/MEASUREMENT_GATE.md`).
- Doc updates at implementation: `docs/MEASUREMENT_GATE.md` (record that the floor is
  characterized and the gate cleared for this floor), `docs/CONTROL_LOOP.md` (Policy
  Behavior note that the shipped profile now enables the floor), and
  `docs/RUNTIME_LOGGING_AND_EVALUATION.md` (the characterization summary), per
  `AGENTS.md` §Change Checklist.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/control-latency-reduction-design-2026-06-18.md`](../control-latency-reduction-design-2026-06-18.md) (D-CADENCE-1) | Adopt the dormant-floor enablement direction; record the characterized floor value and slew thresholds and the measurement evidence that justifies them. | Proposed (settle floor + thresholds from the gate pass before implementation) |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-CADENCE-01 | T, M | `.\scripts\Test-LocalCI.ps1` config-load test that the shipped config sets `poll_tick_floor_ms` in `[25, poll_tick_ms)`; the existing `ComputeCadence` unit tests cover full-slew → floor; (M) a live transient capture shows the effective interval reach the floor. |
| REQ-CADENCE-02 | T | Config-load/`ComputeCadence` tests: `start < full`; relax returns to `poll_tick_ms` after slew subsides; no tighten below `cadence_slew_start_c_per_s`. |
| REQ-CADENCE-03 | M | Characterization capture analyzed with `analyze ingest` + `analyze report`: steady-state achieved interval, `loop_slip_ms`, `loop_overrun`, process CPU% within `MEASUREMENT_GATE.md` exit criteria. |
| REQ-CADENCE-04 | R, M | Review the config diff (`write_cooldown_ms`, channels unchanged); runtime evidence that write frequency is bounded by deadband + cooldown, not the faster floor. |
| REQ-CADENCE-05 | M, R | The `MEASUREMENT_GATE.md` characterization summary exists (AMD/SIO/GPU cadence + fan-write response at the floor); review that the chosen floor/thresholds are justified by it. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| The floor value (`poll_tick_floor_ms`) | implementation (from the gate pass) | `125 ms` candidate (halves detection latency); confirm against the characterization. |
| The slew thresholds (`cadence_slew_start_c_per_s`, `cadence_slew_full_c_per_s`) | implementation | the `ComputeCadence` defaults (`0.5` / `3.0`) as a starting point; refine from observed transient slew. |
| `cadence_relax_per_s` | implementation | the derived default `(poll_tick_ms - poll_tick_floor_ms)/3`; revisit only if relax chatters. |

## 12. Measurement gate & dependencies

- **Measurement gate:** **crossed.** `docs/MEASUREMENT_GATE.md` blocks "Adaptive
  cadence floors below the shipped profile." Required before implementation: a
  characterization of AMD cadence, passive AMD+SIO cadence, GPU telemetry cadence,
  and fan-write response on the current machine at the chosen floor, with full
  per-tick logging, summarized per `docs/RUNTIME_LOGGING_AND_EVALUATION.md` and
  satisfying the gate's Exit Criteria. This is the same standing requirement
  recorded in `docs/next_steps.md` ("Require fresh runtime evidence before changing
  cadence/floor defaults").
- **Depends on:** the cadence engine (`src/control/cadence_score.cpp`) and the tick
  loop (`src/control/tick_runner.cpp`, `src/control/control_scheduler.cpp`
  `WaitForNextControlTick`) — both already implemented. Independent of FEAT-0017 and
  FEAT-0019. Related to the deferred "lower fixed cadence" option, which is the same
  gate (`docs/control-latency-reduction-design-2026-06-18.md` §3, Not promoted).
- **Build/test impact:** a config-load test that the shipped floor is in range and
  the slew thresholds are ordered; the existing `ComputeCadence` unit tests cover
  the math; the characterization run is the gating evidence. Doc updates to
  `MEASUREMENT_GATE.md`, `CONTROL_LOOP.md`, `RUNTIME_LOGGING_AND_EVALUATION.md`.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2 — `F >= P` short-circuit in `cadence_score.cpp`; absent `poll_tick_floor_ms` in shipped config).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4 — the measurement gate is the central stressed invariant).
- [ ] 3. Required design decision record(s) written and marked current (§9 — `docs/control-latency-reduction-design-2026-06-18.md` is Proposed; floor + thresholds await the gate pass).
- [x] 4. Concrete `REQ-CADENCE-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — config-load test, `ComputeCadence` unit tests, characterization measurement, contract review (§10), mirrored in `docs/TRACEABILITY.md`.
- [ ] 6. Confirmed it does not silently move the `MEASUREMENT_GATE.md` baseline — it explicitly **does** cross the gate (§12); this gate is satisfied only when the characterization evidence exists. Open until then.
- [ ] 7. Doctrine check: claims grounded with file paths; proposed values labeled as proposed; `must`/`should`/`is` per `CLAUDE.md` — final read pending the chosen floor/thresholds.

> Held at Draft (gates 3, 6, 7 open): the floor and slew thresholds are undecided
> and the measurement-gate characterization does not yet exist. Promote to Accepted
> only after the gate pass produces the evidence and the decision record settles the
> values.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-CADENCE-01 | | | |
| REQ-CADENCE-02 | | | |
| REQ-CADENCE-03 | | | |
| REQ-CADENCE-04 | | | |
| REQ-CADENCE-05 | | | |

**Spec vs. implementation deltas:** <record at implementation.>
