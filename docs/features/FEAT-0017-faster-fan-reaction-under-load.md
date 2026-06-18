# FEAT-0017: Faster fan reaction under load (control-response retune)

**Project:** svg-mb-control
**Status:** Draft   **Version:** 0.1   **Updated:** 2026-06-18
**Namespace:** `REQ-REACT-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/CONTROL_LOOP.md`, `docs/CONTROL_PIPELINE_MATH.md`,
`docs/response-evaluation-tuning-plan.md`, `docs/COOLING_STRATEGY.md`
**Purpose:** raise how fast a controlled fan ramps toward its commanded duty when
load rises, by retuning the per-channel rise-rate limiter and demand smoothing,
without changing curves, cadence, channel set, or the control-computation
identity.

> Draft / design capture. This is not implementation authorization. The direction
> is recorded in `docs/control-latency-reduction-design-2026-06-18.md`
> (D-REACT-1, Proposed) and is governed by `docs/response-evaluation-tuning-plan.md`.
> Promotion requires the validation pass in §10/§12.

## 1. Summary

On a load step, the controlled fans reach their target duty in tens of seconds.
The audit (`docs/control-latency-reduction-design-2026-06-18.md` §2) shows the
binding term for the bulk of a step is the per-channel rate limiter
(`rise_rate_pct_per_min` together with `max_setpoint_step_pct`,
`src/control/channel_evaluator.cpp` `RateLimitSetpoint`); the demand-smoothing EMA
governs only the approach tail. This feature retunes those
per-channel response values — jointly, lane-targeted, and asymmetrically (faster
rise, unchanged or slower fall) — so a rising thermal demand reaches actuation
sooner while idle noise and spin-down behavior are preserved. It changes config
values only; no curve breakpoint, cadence, channel, or control-identity term
moves.

## 2. Problem & motivation  *(promotion gate 1)*

Named behavior gap, grounded in source and shipped config (not a runtime defect):

1. **The rate limiter, not the EMA, sets reaction speed.** `RateLimitSetpoint`
   caps the per-write move to
   `min(rise_rate_pct_per_min x elapsed/60000, max_setpoint_step_pct)`
   (`src/control/channel_evaluator.cpp`). With the shipped channel `0`
   (`rise_rate_pct_per_min = 75`, `max_setpoint_step_pct = 0.6`,
   `write_cooldown_ms = 250`, `release/control.json`), the effective rise ceiling
   is `min(75/60, 0.6 x 1000/250) = 1.25 %/s`, so the bulk of a `~50%` curve step is
   rate-limited (`~30 s+` to `~55%`; the EMA tail extends full convergence — design
   §2.1).
2. **Raising smoothing alpha alone cannot speed the rate-bound bulk.** The
   rising-path EMA (`ApplyDemandSmoothing`, `src/control/channel_evaluator.cpp`) only
   sets the *target* the rate limiter chases; while `alpha x delta` exceeds the
   rate-limit allowance the written duty is rate-bound regardless of `alpha`
   (`docs/control-latency-reduction-design-2026-06-18.md` §2.1). So a retune that
   moves the bulk of the reaction must raise the rate limiter, and may raise `alpha`
   only for the approach tail.
3. **The two rate knobs interact.** Past `rise_rate_pct_per_min ~= 144` the
   `max_setpoint_step_pct` cap (`2.4 %/s` at the shipped `0.6%`/`250 ms`) becomes
   binding, so raising one without the other stalls at the unraised cap.

The shipped values were chosen for a quiet, no-staircase ramp
(`docs/CONTROL_LOOP.md` Policy Behavior). The gap is that the same conservative
rate also delays a genuine load response; the retune trades a portion of that quiet
margin — magnitude set by the §11 open decision (candidate `~6.7 %/s` on radiator
lanes `1`/`4`/`5`, not chosen) and bounded by the
`response-evaluation-tuning-plan.md` acceptance band (REQ-REACT-04) — for reaction
on the lanes where it matters.

## 3. Goals & non-goals

**Goals**
- Raise the effective rise ceiling on the targeted lanes by raising
  `rise_rate_pct_per_min` and `max_setpoint_step_pct` together, so a rising
  thermal demand actuates sooner.
- Optionally raise `demand_smoothing_rise_alpha` to shorten the approach tail
  after the rate-limited climb.
- Keep the response asymmetric: the falling-direction rate and smoothing stay at
  least as slow as today, so spin-down is not made more aggressive.
- Validate the retune against the `response-evaluation-tuning-plan.md` acceptance
  band (CPU Tctl / GPU memory percentiles, no new authority reasserts, and noise
  within the plan's Pass 1–3 subjective-noise acceptance) before it is adopted.

**Non-goals**
- No change to curve breakpoints, `temp_blend`, channel membership,
  `cpu_override_curve` shape, or the boost overlays. Only the per-channel response
  *rate* values change.
- "Reaction" / "commanded duty" in this spec means the curve-derived base ramp
  (`smoothed_base_setpoint`). The four additive boost overlays (`thermal_pressure` /
  `midband_pressure` / `gpu_airflow` / `cpu_low_soak`) grow at their own per-second
  integrator rates (`src/control/boost_stage.cpp` `UpdateBoostStage`) and remain
  bounded by those unchanged rates — this feature does not accelerate them (design
  §2, "Shape — boost overlays"). The high-temperature response is carried mainly by
  the base curve, which this feature does accelerate.
- No change to `poll_tick_ms`, `poll_tick_floor_ms`, or `write_cooldown_ms` (those
  are FEAT-0018 / the measurement gate).
- No change to the control-computation identity in `CONTROL_PIPELINE_MATH.md`; the
  formulas are unchanged, only their per-channel coefficients.
- No new config key or CLI surface.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| Shipped `250 ms` cadence / channel set / input strategy is the measured baseline | `docs/MEASUREMENT_GATE.md` | Cadence, cooldown, channels, and mixed-input strategy are unchanged. Only per-channel response-rate coefficients move, which the gate does not block; the change is validated through `response-evaluation-tuning-plan.md`, not a new gate pass. |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | `RateLimitSetpoint` and `ApplyDemandSmoothing` are unchanged; only their config inputs change. The identity and operand order are untouched. |
| Acoustic envelope / no-chatter response | `docs/response-evaluation-tuning-plan.md`, `docs/COOLING_STRATEGY.md` | Asymmetric tuning keeps fall behavior at least as slow; deadband / cooldown / authority-reassert are unchanged so no new same-duty writes are introduced; validated against the plan's Pass 1–3 acceptance band. |
| No fan write / authority change outside an explicit live task | `AGENTS.md` §Live Runtime Safety | The control loop already owns these writes. Validation passes are explicit live captures under the plan's stop conditions. |
| Repo stays standalone; runtime sidecar/status schema unchanged | `AGENTS.md` §Repo Boundary, `docs/RUNTIME_HOME.md` | Config-value-only change; no schema, no new field, no external dependency. |

## 5. Behavior specification

The retune lives entirely in `config/control.release.json` (and the byte-identical
`config/control.example.json` where applicable) per-channel fields consumed by
`src/control/channel_evaluator.cpp`. No source change is required for the rate
behavior itself; the only code touched is the config-contract test fixture (§10).

- **Joint rise raise.** On each targeted lane, `rise_rate_pct_per_min` and
  `max_setpoint_step_pct` are raised together so the effective rise ceiling
  `min(rise_rate_pct_per_min/60, max_setpoint_step_pct x 1000/write_cooldown_ms)`
  increases to the chosen target. The accepted target (lanes and `%/s`) is recorded
  in the decision record before implementation (§9/§11).
- **Tail smoothing.** `demand_smoothing_rise_alpha` may be raised on the same lanes
  to reduce the post-climb approach lag. The fall-direction
  `demand_smoothing_fall_alpha` and `decay_latch_*` stay at least as slow as the
  current shipped values.
- **Asymmetry preserved.** `fall_rate_pct_per_min` is not raised above its current
  shipped value on any lane (spin-down is not made faster by this feature).
- **Lane targeting.** The retune is applied first to the radiator lanes
  (`1`/`4`/`5`), consistent with `response-evaluation-tuning-plan.md` authority
  bias; the front 200 mm intake pair (`2`/`3`) keeps its `>= 4%` spacing and
  dynamic low-end curve unchanged.
- **No-chatter guard.** Deadband, cooldown, and the authority-reassert path
  (`CONTROL_PIPELINE_MATH.md` §9) are unchanged, so the faster rate produces larger
  intermediate steps, not additional same-duty writes.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-REACT-01 | On each retuned lane, `rise_rate_pct_per_min` and `max_setpoint_step_pct` must both be raised relative to the current shipped value so the effective rise ceiling `min(rise_rate_pct_per_min/60, max_setpoint_step_pct x 1000/write_cooldown_ms)` increases; a change that raises only one (leaving the other as the binding cap) does not satisfy this requirement. |
| REQ-REACT-02 | `demand_smoothing_rise_alpha` may be raised on a retuned lane, but the fall-direction response (`fall_rate_pct_per_min`, `demand_smoothing_fall_alpha`, `decay_latch_pct_per_min`) must not be made faster than its current shipped value: the retune is rise-asymmetric. |
| REQ-REACT-03 | The retune must not change curve breakpoints, `temp_blend`, channel membership, `cpu_override_curve`, the boost overlays, `poll_tick_ms`, `write_cooldown_ms`, or `deadband_pct`; only per-channel rise-rate and rise-smoothing values change, so no measurement-gate boundary is crossed and the control-computation identity is unchanged. |
| REQ-REACT-04 | Before adoption, a combined CPU+GPU validation pass (`response-evaluation-tuning-plan.md` Pass 3) must show the first-duty-increase time and ramp time improve versus the pre-retune capture (measured from `last_raw_demand_pct` / `last_smoothed_demand_pct` / setpoint and `channelN_feedforward_pct`), while CPU Tctl and GPU memory percentiles stay within the plan's acceptance band and no new `control_loop.authority_reasserted` events appear after startup. |
| REQ-REACT-05 | The front 200 mm intake pair (channels `2`, `3`) must retain its `>= 4%` low-end spacing (`tests/test_config_contracts.py::test_release_intake_low_end_curves_follow_machine_policy`) after the retune; the feature must not collapse or re-pin those lanes. |

## 7. Data / schema deltas

- New/changed fields: none. Existing per-channel keys
  (`rise_rate_pct_per_min`, `fall_rate_pct_per_min`, `max_setpoint_step_pct`,
  `demand_smoothing_rise_alpha`, `demand_smoothing_fall_alpha`) take new values.
- Config impact (`config/control.*.json`, `config/machines/*.json`): updated
  per-channel values in `config/control.release.json` (and
  `config/control.example.json` if it tracks the same lanes). No machine-policy
  change unless the decision records one.
- Schema/version impact: none. No `schema_version` bump; no runtime-home,
  manifest, CSV, or status field changes. Existing archives stay valid.

## 8. CLI / config / operator surface deltas

- No new CLI subcommand, flag, or config key. The change is values inside existing
  keys.
- Doc updates at implementation: `docs/response-evaluation-tuning-plan.md` (record
  the retune iteration and its validation evidence) and
  `docs/CONTROL_PIPELINE_MATH.md` §13 (real-data validation note), per
  `AGENTS.md` §Change Checklist. `README.md` only if a documented default it states
  changes.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/control-latency-reduction-design-2026-06-18.md`](../control-latency-reduction-design-2026-06-18.md) (D-REACT-1) | Adopt the lane-targeted, joint, asymmetric rise retune; record the target lanes and the chosen effective `%/s` ceiling, and that fall behavior is not made faster. | Proposed (settle lanes + target before implementation) |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-REACT-01 | T, R | `.\scripts\Test-LocalCI.ps1` config-contract test asserting each retuned lane raised both `rise_rate_pct_per_min` and `max_setpoint_step_pct` (effective ceiling rose); review the identity vs `docs/control-latency-reduction-design-2026-06-18.md` §2.1. |
| REQ-REACT-02 | T, R | Config-contract test asserting no retuned lane raised `fall_rate_pct_per_min` / `demand_smoothing_fall_alpha` / `decay_latch_pct_per_min` above the shipped value; review for rise-asymmetry. |
| REQ-REACT-03 | R | Review the config diff vs `docs/MEASUREMENT_GATE.md` and `docs/CONTROL_PIPELINE_MATH.md`: curves, blend, channels, cadence, cooldown, deadband, overlays unchanged. |
| REQ-REACT-04 | M | Live combined-load capture analyzed with `svg-mb-control analyze ingest` + `analyze report` (Pass 3): before/after reaction time improved; CPU Tctl / GPU memory percentiles within the acceptance band; no post-startup authority reasserts. |
| REQ-REACT-05 | T | `tests/test_config_contracts.py::test_release_intake_low_end_curves_follow_machine_policy` stays green (channels `2`/`3` keep `>= 4%` spacing). |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Which lanes are retuned and to what effective `%/s` ceiling | implementation | radiator lanes `1`/`4`/`5`; a `~6 s` ramp for a `~40%` step (`~6.7 %/s`) is the candidate, not chosen. |
| Whether `demand_smoothing_rise_alpha` is raised, and by how much | implementation | raise on the retuned lanes only after the rate raise; magnitude set from the Pass-3 tail behavior. |
| Whether the front 200 mm intake pair (`2`/`3`) is retuned at all | implementation | no — keep their dynamic low-end curve and `4%` spacing; retune radiator lanes first. |

## 12. Measurement gate & dependencies

- **Measurement gate:** not crossed. Cadence, cooldown, channels, and mixed-input
  strategy are unchanged (`docs/MEASUREMENT_GATE.md` blocks faster cadence/cooldown,
  adaptive floors, and channel additions — none of which this feature touches). The
  governing evidence is the `response-evaluation-tuning-plan.md` acceptance band,
  not a new gate pass.
- **Depends on:** the per-channel evaluation path (`src/control/channel_evaluator.cpp`
  `RateLimitSetpoint`, `ApplyDemandSmoothing`) and the shipped config
  (`config/control.release.json`). Independent of FEAT-0018 and FEAT-0019; may land
  in any order.
- **Build/test impact:** a config-contract test under `tests/` asserting the joint
  raise and the rise-asymmetry; doc updates to
  `docs/response-evaluation-tuning-plan.md` and `docs/CONTROL_PIPELINE_MATH.md` §13.
  No `src/` behavior change.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2 — source + shipped-config rate-ceiling analysis; corroborated by `docs/control-latency-reduction-design-2026-06-18.md`).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [ ] 3. Required design decision record(s) written and marked current (§9 — `docs/control-latency-reduction-design-2026-06-18.md` is Proposed; lanes + target ceiling not yet settled).
- [x] 4. Concrete `REQ-REACT-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — config-contract test, response-evaluation Pass 3, contract review (§10), mirrored in `docs/TRACEABILITY.md`.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline (config-value-only; cadence/channels unchanged).
- [ ] 7. Doctrine check: behavior claims grounded with file paths; proposed values labeled as proposed; `must`/`should`/`is` per `CLAUDE.md` — pending a final read once the lanes/targets are chosen.

> Held at Draft (gates 3 and 7 open): the lane set and target `%/s` ceiling are an
> open decision, and the validation evidence (REQ-REACT-04) does not yet exist.
> Promote to Accepted only after the decision record settles the targets and a
> Pass-3 capture confirms the envelope.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-REACT-01 | | | |
| REQ-REACT-02 | | | |
| REQ-REACT-03 | | | |
| REQ-REACT-04 | | | |
| REQ-REACT-05 | | | |

**Spec vs. implementation deltas:** <record at implementation.>
