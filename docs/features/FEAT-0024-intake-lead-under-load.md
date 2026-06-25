# FEAT-0024: Intake-lead fan response under load

**Project:** svg-mb-control
**Status:** Draft   **Version:** 0.1   **Updated:** 2026-06-25
**Namespace:** `REQ-INLEAD-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/CONTROL_LOOP.md`, `docs/CONTROL_PIPELINE_MATH.md`,
`docs/response-evaluation-tuning-plan.md`, `docs/COOLING_STRATEGY.md`,
`docs/features/FEAT-0017-faster-fan-reaction-under-load.md`
**Purpose:** make the intake lanes (`2`/`3`/`4`) lead the exhaust lanes when a
thermal load rises — engage earlier, ramp faster, and hold — by a config-only,
rise-asymmetric retune of the intake per-channel rate limiter, `gpu_airflow`
onset/ceiling, and `cpu_override` climb knees, without changing idle behavior,
curves below `72 C`, cadence, channels, or the control-computation identity.

> Draft / design capture. This is not implementation authorization. The direction
> is recorded in `docs/intake-lead-response-decision-2026-06-25.md` (Current for
> direction; candidate magnitudes settled by Pass-3) and is governed by
> `docs/response-evaluation-tuning-plan.md`. Promotion requires the validation
> pass in §10/§12.

## 1. Summary

As a thermal load rises, the intake lanes should supply fresh airflow **first**.
Today they do not: on a rising transient the front-radiator intake `4` is the
single slowest-ramping channel in the system (`rise_rate_pct_per_min = 60`,
`demand_smoothing_rise_alpha = 0.008`), and the intake `cpu_override` curves are
flat through the mid band, so the intakes sit near their floor under load while
the exhaust lanes carry the dynamic range (`docs/control-latency-reduction-design-2026-06-18.md`;
control-pipeline analysis 2026-06-25). This feature retunes the intake lanes'
per-channel response **values** so they engage earlier (lower `gpu_airflow_start_c`
than the exhausts), ramp harder (raised rate limiter + raised intake boost
ceilings + steeper intake `cpu_override` mid-band), and hold — a level-based
**surge-and-hold**, config-only, rise-asymmetric (fall unchanged). Idle, the
curves below `72 C`, the exhaust lanes, cadence, channels, and the
control-computation identity do not change.

## 2. Problem & motivation  *(promotion gate 1)*

Named behavior gap, grounded in source, shipped config, and live evidence:

1. **An intake lane is the slowest channel in the system.** Channel `4`
   (front-radiator Noctua intake) ships with `rise_rate_pct_per_min = 60` and
   `demand_smoothing_rise_alpha = 0.008`, the lowest of all six lanes; its
   combined smoothing+rate step response (`t63`) to a `30%` raw step is ~31 s
   versus ~21 s for the exhaust lanes (`config/control.release.json`;
   `src/control/channel_evaluator.cpp` `RateLimitSetpoint` / `ApplyDemandSmoothing`;
   control-pipeline analysis 2026-06-25). An intake meant to supply air on load
   lags every exhaust.
2. **Intakes are parked under load.** The intake `cpu_override_curve` is flat
   through the mid band (e.g. channel `2`: `72 C -> 64%`, `82 C -> 64%`), so on a
   CPU climb the intakes barely rise while the exhaust lanes `1`/`5` climb steeply
   (`+50%` over `72-92 C`). Live mid-load evidence (2026-06-25): intakes moved
   `60 -> 63.5%` / `56 -> 59.6%` while the rear exhaust swung `15.5 -> 36.9%`.
3. **No intake-first sequencing exists.** All four pressure boosts are
   absolute-temperature **level** integrators, so lanes that share a temperature
   source cross their thresholds simultaneously; the only intake-favoring boost by
   magnitude is `gpu_airflow`, and it shares the same onset band as the exhausts
   (`docs/CONTROL_PIPELINE_MATH.md` §6; `src/control/boost_stage.cpp`). Nothing
   makes the intakes engage before the exhausts.

The shipped per-channel rate and curve values were tuned for a quiet ramp and for
radiator-lane CPU authority; the gap is that the same tuning leaves the **intake**
lanes lagging on a rising load. The operator's standing request (2026-06-25) is
that the intakes lead. Idle resonance / steadiness is explicitly out of scope for
this feature (operator: "idle's basically fine").

## 3. Goals & non-goals

**Goals**
- On a rising CPU or GPU load, the intake lanes (`2`/`3`/`4`) reach
  first-duty-increase and complete their ramp **sooner than** the exhaust lanes
  (`0`/`1`/`5`), measured from the control-loop CSV.
- Raise the intake effective rise ceiling
  `min(rise_rate_pct_per_min/60, max_setpoint_step_pct x 1000/write_cooldown_ms)`
  by raising `rise_rate_pct_per_min` and `max_setpoint_step_pct` together
  (largest raise on the slowest lane `4`).
- Make the intakes engage on a GPU climb before the exhausts by lowering the
  intake `gpu_airflow_start_c` below the exhaust value and raising the intake
  `gpu_airflow_max_boost_pct`.
- Steepen the front-radiator-intake (`4`) `cpu_override` **mid band** (`72-86 C`)
  so it climbs earlier on a CPU load (it directly cools the AIO and has the most
  headroom below its high-end knots), keeping the surge level-held (no
  recover-down); lanes `2`/`3` lead via the rate raise and `gpu_airflow` onset.

**Non-goals**
- No change to any intake curve or `cpu_override` knot at or below `72 C`, no idle
  re-spacing, and no change to idle steadiness — idle is out of scope (the
  `gpu_airflow_start_c = 58 C` onset is above the idle GPU envelope and does not
  fire at idle).
- No change to the exhaust lanes `0`/`1`/`5`, to the radiator no-mirror / `>= 2pt`
  GPU stagger / floors-above-rear contracts, or to the radiator-exhaust
  highest-CPU authority at the extreme (the channel `4` `cpu_override` knots
  `>= 90 C` stay unchanged, so channels `1`/`5` keep exceeding the intakes at
  `>= 92 C`).
- No faster spin-down: no falling-direction value
  (`fall_rate_pct_per_min`, `demand_smoothing_fall_alpha`,
  `decay_latch_pct_per_min`) is raised on any lane.
- No genuine rate-of-rise (dT/dt) "overshoot" term — that would need new code and
  a control-identity change; see `docs/intake-lead-response-decision-2026-06-25.md`
  D-INLEAD-2. This feature is a level-based surge.
- No change to `poll_tick_ms`, `poll_tick_floor_ms`, `write_cooldown_ms`,
  `deadband_pct`, the channel set, `temp_blend`, or the control-computation
  identity in `CONTROL_PIPELINE_MATH.md` (only per-channel coefficient values
  change). No new config key or CLI surface.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| Shipped `250 ms` cadence / channel set / mixed-input strategy is the measured baseline | `docs/MEASUREMENT_GATE.md` | Cadence, cooldown, deadband, channels, and `temp_blend` are unchanged; only per-channel rise-rate, `gpu_airflow`, and `cpu_override` mid-band values on intake lanes move. Validated through `response-evaluation-tuning-plan.md`, not a new gate pass. |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | `RateLimitSetpoint`, `ApplyDemandSmoothing`, and `UpdateBoostStage` are unchanged; only their per-channel config inputs change. The identity and operand order are untouched. |
| Acoustic / no-chatter and asymmetric spin-down | `docs/response-evaluation-tuning-plan.md`, `docs/COOLING_STRATEGY.md` | Falling-direction values are not raised; deadband / cooldown / authority-reassert unchanged, so a faster rise yields larger intermediate steps, not extra same-duty writes. |
| Front-200 mm resonance spacing and dynamic soft floor | `docs/COOLING_STRATEGY.md`, `tests/test_machine_cooling_policy.py`, `tests/test_config_contracts.py` | No intake knot `<= 72 C` changes, so the `>= 4%` ch2/ch3 spacing and soft-floor-not-static contracts are byte-unchanged. |
| Radiator no-mirror / stagger / floors-above-rear and radiator CPU authority | `docs/COOLING_STRATEGY.md`, `tests/test_machine_cooling_policy.py`, `tests/test_config_contracts.py` | The exhaust lanes `0`/`1`/`5` are not touched; the intake `>= 88 C` `cpu_override` knots stay below the channel `1`/`5` knees. |
| No fan write / authority change outside an explicit live task | `AGENTS.md` §Live Runtime Safety | The control loop already owns these writes; validation and deploy are explicit live captures under the plan's stop conditions. |
| Repo stays standalone; runtime sidecar/status schema unchanged | `AGENTS.md` §Repo Boundary, `docs/RUNTIME_HOME.md` | Config-value-only change; no schema, no new field, no external dependency. |

## 5. Behavior specification

The retune lives entirely in `config/control.release.json` (and the packaged
`release/control.json`) per-channel fields consumed by
`src/control/channel_evaluator.cpp` and `src/control/boost_stage.cpp`. No `src/`
behavior change is required; the only code touched is config-contract test
fixtures (§10). Candidate magnitudes are in
`docs/intake-lead-response-decision-2026-06-25.md` §3 and are settled by Pass-3.

- **Joint intake rise raise.** On lanes `2`/`3`/`4`, `rise_rate_pct_per_min` and
  `max_setpoint_step_pct` are raised together so the effective rise ceiling
  increases (largest on lane `4`, the slowest channel). The exhaust lanes keep
  their shipped values, so after the change the intakes ramp faster than the
  exhausts.
- **Intake-first GPU onset.** `gpu_airflow_start_c` on the intake lanes is lowered
  below the exhaust value and `gpu_airflow_max_boost_pct` is raised, so on a GPU
  climb the intakes begin boosting before the exhausts and surge to a higher
  ceiling. The onset stays above the idle GPU envelope so it does not fire at
  idle.
- **Radiator-intake mid-band climb.** The channel `4` `cpu_override_curve` is
  steepened in the `72-86 C` band so the radiator intake climbs earlier on a CPU
  load instead of parking; its knots `<= 72 C` and `>= 90 C` are unchanged (idle
  untouched; the shipped top-end ordering preserved). Lanes `2`/`3`
  `cpu_override` are unchanged.
- **Surge-and-hold.** Because every retuned term is level-based, the intake duty
  rises with temperature and **holds** while temperature stays up; there is no
  rise-above-then-recover. Optionally `demand_smoothing_rise_alpha` is raised on
  lane `4` to shorten its approach tail.
- **Asymmetry / no-chatter guard.** Falling-direction values and the
  deadband / cooldown / authority-reassert path are unchanged
  (`CONTROL_PIPELINE_MATH.md` §9).

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-INLEAD-01 | On each intake lane (`2`, `3`, `4`), `rise_rate_pct_per_min` and `max_setpoint_step_pct` must both be raised relative to the current shipped value so the effective rise ceiling `min(rise_rate_pct_per_min/60, max_setpoint_step_pct x 1000/write_cooldown_ms)` increases, and neither knob is left as the sole binding cap; lane `4` must receive a rise raise at least as large as lanes `2`/`3`. |
| REQ-INLEAD-02 | On each intake lane, `gpu_airflow_start_c` must be lower than the exhaust lanes' `gpu_airflow_start_c` and `gpu_airflow_max_boost_pct` must be at least the exhaust lanes' value, so the intakes engage and surge on a GPU climb ahead of the exhausts; the intake `gpu_airflow_start_c` must stay above the idle GPU envelope so it does not fire at idle. |
| REQ-INLEAD-03 | The front-radiator-intake channel `4` `cpu_override_curve` must be steepened only in the `72-86 C` band so it climbs earlier on a CPU load: its knots at or below `72 C` and at or above `90 C` must be byte-unchanged (preserving idle and the shipped top-end ordering, where channels `1`/`5` exceed the intakes at `>= 92 C`), and the curve must stay monotonic non-decreasing. The channel `2`/`3` `cpu_override_curve`s are unchanged — those lanes lead via the rate raise and the `gpu_airflow` onset, not via CPU-override steepening. |
| REQ-INLEAD-04 | The retune must not change any intake curve, `cpu_override`, or soft-floor knot at or below `72 C`, `min_duty_pct`, `temp_blend`, channel membership, `poll_tick_ms`, `write_cooldown_ms`, or `deadband_pct`; the front-200 mm `>= 4%` spacing and soft-floor-not-static contracts and the exhaust-lane no-mirror / stagger / floors-above-rear contracts must stay green, so no measurement-gate boundary is crossed and idle is unchanged. |
| REQ-INLEAD-05 | The retune must be rise-asymmetric: no `fall_rate_pct_per_min`, `demand_smoothing_fall_alpha`, or `decay_latch_pct_per_min` value is raised on any lane, and the exhaust lanes `0`/`1`/`5` are byte-unchanged. |
| REQ-INLEAD-06 | Before adoption, a combined CPU+GPU validation pass (`response-evaluation-tuning-plan.md` Pass 3) must show the intake lanes reach first-duty-increase and complete their ramp sooner than the exhaust lanes on a rising load, while CPU Tctl and GPU memory percentiles stay within the plan's acceptance band and no new `control_loop.authority_reasserted` events appear after startup; a Pass-1 idle hold must show idle per-channel setpoint / RPM unchanged versus the pre-change baseline. |

## 7. Data / schema deltas

- New/changed fields: none. Existing per-channel keys on lanes `2`/`3`/`4`
  (`rise_rate_pct_per_min`, `max_setpoint_step_pct`, `gpu_airflow_start_c`,
  `gpu_airflow_max_boost_pct`, `cpu_override_curve` `72-95 C` knots, and
  optionally `demand_smoothing_rise_alpha`) take new values.
- Config impact (`config/control.*.json`, `config/machines/*.json`): updated
  per-channel values in `config/control.release.json` and the packaged
  `release/control.json`. The machine policy JSON `reference_static_low_load_rpm`
  baseline and spacing fields are **not** changed (idle untouched); the policy
  `response_intent` prose for the intake lanes is updated at implementation to
  describe the lead behavior.
- Schema/version impact: none. No `schema_version` bump; no runtime-home,
  manifest, CSV, or status field change. Existing archives stay valid.

## 8. CLI / config / operator surface deltas

- No new CLI subcommand, flag, or config key. The change is values inside existing
  per-channel keys.
- Doc updates at implementation (`AGENTS.md` §Change Checklist):
  `docs/response-evaluation-tuning-plan.md` (record the retune iteration and its
  Pass-1/Pass-3 evidence), `docs/COOLING_STRATEGY.md` and
  `config/machines/snd-desk.cooling.policy.json` `response_intent` (intake lead
  prose), and `docs/CONTROL_PIPELINE_MATH.md` §13 (real-data validation note).
  `README.md` only if a documented default it states changes (none expected).

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/intake-lead-response-decision-2026-06-25.md`](../intake-lead-response-decision-2026-06-25.md) | The lead lanes (intakes `2`/`3`/`4`), the surge-and-hold (config-only, not a dT/dt overshoot) mechanism, idle out of scope, rise-asymmetry, and radiator-authority preservation. Candidate magnitudes are settled by Pass-3. | Current (direction); magnitudes pending Pass-3 |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-INLEAD-01 | T, R | `.\scripts\Test-LocalCI.ps1` config-contract test asserting each intake lane raised both `rise_rate_pct_per_min` and `max_setpoint_step_pct` (effective ceiling rose) and lane `4`'s rise raise `>=` lanes `2`/`3`; review vs `docs/intake-lead-response-decision-2026-06-25.md` §3. |
| REQ-INLEAD-02 | T, R | Config-contract test asserting intake `gpu_airflow_start_c` `<` exhaust `gpu_airflow_start_c`, intake `gpu_airflow_max_boost_pct` `>=` exhaust value, and intake onset `>` idle GPU envelope; review. |
| REQ-INLEAD-03 | T, R | Config-contract test asserting the channel `4` `cpu_override_curve` knots `<= 72 C` and `>= 90 C` are byte-unchanged, the curve is monotonic non-decreasing, the `72-86 C` band is steepened, and the `2`/`3` `cpu_override`s are unchanged; review of the band. |
| REQ-INLEAD-04 | T | `tests/test_machine_cooling_policy.py` and `tests/test_config_contracts.py` stay green (front-pair `>= 4%`, soft-floor-not-static, no-mirror/stagger/floors-above-rear, topology) plus a contract assertion that no intake knot `<= 72 C`, `min_duty_pct`, cadence, cooldown, or deadband changed. |
| REQ-INLEAD-05 | T, R | Config-contract test asserting no `fall_rate_pct_per_min` / `demand_smoothing_fall_alpha` / `decay_latch_pct_per_min` raised on any lane and the exhaust lanes `0`/`1`/`5` byte-unchanged; review for rise-asymmetry. |
| REQ-INLEAD-06 | M | Live Pass-3 combined-load capture analyzed with `svg-mb-control analyze ingest` + `analyze report`: intake first-duty / ramp time precedes the exhausts; CPU Tctl / GPU memory percentiles within the acceptance band; no post-startup authority reasserts; Pass-1 idle hold shows idle unchanged. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| The exact retune magnitudes (rise/step per lane, `gpu_airflow` onset/ceiling, the `72-86 C` `cpu_override` knots) | adoption | the candidate set in `docs/intake-lead-response-decision-2026-06-25.md` §3; settled by Pass-3 within the acceptance band |
| Whether lane `4` `demand_smoothing_rise_alpha` is raised, and by how much | adoption | raise to ~`0.014` only after the rate raise; magnitude from the Pass-3 tail behavior |
| Whether the intake mid-band steepening uses `cpu_override` only or also the primary GPU `curve` knots | implementation | `cpu_override` mid-band only; revisit if Pass-3 shows the GPU path under-leads |

## 12. Measurement gate & dependencies

- **Measurement gate:** not crossed. Cadence, cooldown, deadband, channels, and
  `temp_blend` are unchanged (`docs/MEASUREMENT_GATE.md` blocks faster
  cadence/cooldown, adaptive floors, and channel additions — none touched). The
  governing evidence is the `response-evaluation-tuning-plan.md` Pass-1/Pass-3
  acceptance band.
- **Depends on:** the per-channel evaluation path
  (`src/control/channel_evaluator.cpp`, `src/control/boost_stage.cpp`) and the
  shipped config. Reuses the FEAT-0017 rate-limit identity (REQ-REACT-01/02);
  independent of FEAT-0017's radiator-lane scope and may land in any order.
- **Build/test impact:** config-contract tests under `tests/` for the intake
  joint raise, the GPU-onset lead, the `cpu_override` mid-band band, and the
  rise-asymmetry; doc updates per §8. No `src/` behavior change; no release build
  required for the config edit itself (`AGENTS.md` §Change Checklist — a live
  deploy is a separate explicit live task).

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2 — source + shipped-config + live-evidence analysis 2026-06-25).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [x] 3. Required design decision record(s) written and marked current (§9 — `docs/intake-lead-response-decision-2026-06-25.md` Current for direction; magnitudes settled by Pass-3).
- [x] 4. Concrete `REQ-INLEAD-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — config-contract tests, response-evaluation Pass-1/Pass-3, contract review (§10), mirrored in `docs/TRACEABILITY.md`.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline (config-value-only; cadence/channels/deadband unchanged).
- [x] 7. Doctrine check: behavior claims grounded with file paths; proposed values labeled as candidates; `must`/`should`/`is` per `CLAUDE.md`.

> Held at Draft: the retune magnitudes are an open decision (§11) settled by the
> response-evaluation Pass-3, and the validation evidence (REQ-INLEAD-06) does not
> yet exist. Promote to Accepted only after the Pass-1/Pass-3 captures confirm the
> intake-lead margin and the unchanged-idle envelope.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-INLEAD-01 | | | |
| REQ-INLEAD-02 | | | |
| REQ-INLEAD-03 | | | |
| REQ-INLEAD-04 | | | |
| REQ-INLEAD-05 | | | |
| REQ-INLEAD-06 | | | |

**Spec vs. implementation deltas:** <record at implementation.>
