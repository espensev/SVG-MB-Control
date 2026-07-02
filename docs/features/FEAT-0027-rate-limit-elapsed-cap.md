# FEAT-0027: Rate-limiter elapsed cap (loop-jitter-robust slew)

**Project:** svg-mb-control
**Status:** Draft   **Version:** 0.1   **Updated:** 2026-06-25
**Namespace:** `REQ-SLEWCAP-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/CONTROL_LOOP.md`, `docs/CONTROL_PIPELINE_MATH.md`,
`docs/response-evaluation-tuning-plan.md`
**Purpose:** bound the `elapsed_since_last_write` used in the per-channel rate
budget so that a slipped/overrun control tick cannot produce an oversized,
overshooting fan step — decoupling fan-setpoint smoothness from control-loop timing
jitter, with byte-identical behavior at nominal cadence.

> Draft / design capture. This is not implementation authorization. The direction is
> recorded in `docs/ratelimit-elapsed-cap-decision-2026-06-25.md` (Current); the
> forensic + replay evidence is in `docs/intake-lead-grounding-2026-06-25.md`.
> Promotion requires the validation in §10/§12.

## 1. Summary

`RateLimitSetpoint` sizes each fan step by a rate budget
`α = min(rate × elapsed_since_last_write / 60000, max_setpoint_step_pct)`
(`src/control/channel_evaluator.cpp:335-380`, `docs/CONTROL_PIPELINE_MATH.md` §8.1).
At a uniform `250 ms` tick the step is ~`0.375 %`; when a tick slips
(`elapsed_since_last_write` grows to hundreds of ms or seconds), the budget grows in
proportion and the setpoint overshoots the curve target, then corrects on the next
tick — an audible direction-reversal "hunting" the operator hears as the fans being
"less tight." This feature adds a config bound `rate_limit_max_elapsed_ms` so the
budget uses `min(elapsed_since_last_write, rate_limit_max_elapsed_ms)`. Set just
above the nominal tick (candidate `300 ms`), it is inactive at normal cadence
(identical output) and clips only the slipped-tick tail, eliminating the hunting
without slowing the up-response.

## 2. Problem & motivation  *(promotion gate 1)*

Grounded in a forensic timeline and a validated replay
(`docs/intake-lead-grounding-2026-06-25.md`):

1. **Loop-timing regression, not a config change.** Across all sessions the control
   config is byte-identical (`config_sha256 = 45a0a1c7`). Fan tightness degraded at
   the `ba83aed` rebuild deployed 2026-06-25: `loop_slip` ~`1.2 ms → up to 13 ms`,
   `loop_work` p99 `~50 ms → 70–191 ms`, intervals spiking to `3.58 s`, worst during
   the energy/sweeper live-M captures (off-thread sweeper PawnIO/`Global\Access_PCI`
   mutex contention with the control loop's AMD reads).
2. **The elapsed-based rate budget converts loop slip into overshoot.** Because
   `α` scales with `elapsed_since_last_write` (`channel_evaluator.cpp:370`), a
   slipped tick authorizes a larger step that overshoots the noisy curve target and
   reverses next tick. A validated replay (model error 0.10–0.12 % vs the logged
   setpoint, fed the real per-tick `loop_achieved_interval_ms`) shows direction
   reversals of `58–72 / 1000` in the loaded band on the worst-slip session.
3. **The shipped `max_setpoint_step_pct` does not cover this.** It bounds the
   absolute step magnitude but not the elapsed-driven overshoot-then-reverse pattern;
   on the measured window the steps stayed below the cap yet still reversed.

The current behavior is correct only when the loop holds its `250 ms` period; it is
fragile to any source of slip (telemetry, captures, external mutex contention). The
gap is a latent rate-limiter fragility, surfaced by the timing regression. A live
cap-off capture under CPU-stress (2026-06-25, current config `c5b5cb21`, sweeper off)
independently reconfirms the elapsed→step coupling on hardware (max up-step `0.80 %`
p50 / `1.16 %` max at `loop_slip > 1 s` vs `~0 %` at nominal cadence) and shows the
slip recurs from plain CPU saturation alone — see
`docs/ratelimit-elapsed-cap-decision-2026-06-25.md` §3b.

## 3. Goals & non-goals

**Goals**
- Bound the `elapsed_since_last_write` used in the `RateLimitSetpoint` rate budget by
  a new config `rate_limit_max_elapsed_ms`, so a slipped tick cannot enlarge the step.
- Preserve identity when the cap is unset/non-positive, and (with the cap set just
  above the nominal tick) preserve byte-identical output at nominal cadence
  (`elapsed ≤ cap`).
- Apply the bound uniformly to every `RateLimitSetpoint` caller (curve law, the
  FEAT-0003 PID law, and the low-band stage-boost rate limiter).
- Update the control-computation identity in `docs/CONTROL_PIPELINE_MATH.md` §8.1.
- Validate live that the slip-induced reversal / step-irregularity drops with the cap
  enabled and the up-response is unchanged.

**Non-goals**
- No change to `poll_tick_ms`, `write_cooldown_ms`, `deadband_pct`, the channel set,
  `temp_blend`, curves, `cpu_override_curve`, the boost overlays, the demand-smoothing
  EMA, or `max_setpoint_step_pct`.
- No demand-side hysteresis or rate-of-rise/delta term (the shelved idea; the
  validated data showed steady-state was already steady and the culprit was timing).
- No attempt to fix the loop slip itself (telemetry/sweeper contention) — that is a
  separate operational concern; this feature makes the slew robust to it.
- Not coupled to FEAT-0024 (intake-lead); independent and may land in any order.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | §8.1 is amended to `α = min(ρ·min(Δt, Δt_max)/60000, δ_max)`; the doc and the operand order are updated in lockstep, and identity is preserved when `Δt_max` is unset. |
| Shipped `250 ms` cadence / cooldown / channel set / deadband is the measured baseline | `docs/MEASUREMENT_GATE.md` | Cadence, cooldown, deadband, and channels are unchanged. The cap bounds only the rate-budget elapsed; at nominal cadence (`elapsed ≤ cap`) output is byte-identical, so no gate boundary moves. |
| Acoustic envelope / no-chatter; asymmetric spin-down | `docs/response-evaluation-tuning-plan.md`, `docs/COOLING_STRATEGY.md` | The cap reduces slip-induced reversals (improves the envelope) and is symmetric on rise/fall budget; it never makes spin-down faster than the configured `fall_rate`. |
| Shared rate limiter / FEAT-0003 D6 live-PID slew gate stays intact | `docs/CONTROL_PIPELINE_MATH.md` §8.1, `docs/features/FEAT-0003-*` | The cap only narrows the rate-budget elapsed; `max_setpoint_step_pct` (the D6 gate's per-tick cap) is unchanged, so the live-PID slew gate is not weakened. |
| No fan write / authority change outside an explicit live task | `AGENTS.md` §Live Runtime Safety | The control loop already owns these writes; the validation and any live enable are explicit live tasks. |
| Repo stays standalone; runtime sidecar/status schema unchanged | `AGENTS.md` §Repo Boundary, `docs/RUNTIME_HOME.md` | One new config scalar; no schema/sidecar/status/CSV field change. |

## 5. Behavior specification

The change is in `src/control/channel_evaluator.cpp` `RateLimitSetpoint` and the
config that feeds it.

- **Capped elapsed.** `RateLimitSetpoint` gains a `max_elapsed_ms` input. The rate
  budget becomes `max_allowed = rate × min(elapsed_ms, max_elapsed_ms) / 60000` when
  `max_elapsed_ms` is finite and positive; otherwise the elapsed is uncapped
  (current behavior). The subsequent `min(..., max_setpoint_step_pct)` and the
  `|delta| ≤ max_allowed` short-circuit are unchanged.
- **Config field.** A loop-level `control_loop.rate_limit_max_elapsed_ms` (double, ms)
  is parsed and validated like the other optional numeric controls (NaN/absent →
  feature inert). It is threaded into every `RateLimitSetpoint` call: the per-channel
  setpoint, the low-band stage boost, and the FEAT-0003 PID path.
- **Shipped value.** `config/control.release.json` sets `rate_limit_max_elapsed_ms`
  to the validated value (candidate `300 ms` = `poll_tick_ms` + margin), so the cap is
  inactive at nominal cadence and clips only slipped/accumulated-elapsed ticks.
- **No other stage changes.** Smoothing, boosts, deadband, cooldown, breaker,
  baseline restore, and `max_setpoint_step_pct` are untouched.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-SLEWCAP-01 | `RateLimitSetpoint` must compute the rate budget from `min(elapsed_since_last_write_ms, rate_limit_max_elapsed_ms)` when `rate_limit_max_elapsed_ms` is finite and positive, and from the uncapped elapsed otherwise; with the cap unset, the returned setpoint must equal the pre-feature result for every input (identity). |
| REQ-SLEWCAP-02 | The cap must bound only the rate-budget elapsed. `max_setpoint_step_pct`, `deadband_pct`, `write_cooldown_ms`, `poll_tick_ms`, the channel set, curves, `cpu_override_curve`, the boost overlays, and the demand-smoothing EMA must be unchanged; the fall-direction rate must not be made faster. |
| REQ-SLEWCAP-03 | When `elapsed_since_last_write_ms ≤ rate_limit_max_elapsed_ms` (nominal cadence), the rate-limited output must be byte-identical to the uncapped path; only ticks with `elapsed > cap` may differ. |
| REQ-SLEWCAP-04 | The cap must apply uniformly to every `RateLimitSetpoint` caller — the per-channel setpoint, the low-band stage-boost rate limiter, and the FEAT-0003 PID law — and `docs/CONTROL_PIPELINE_MATH.md` §8.1 must be amended to the capped identity in the same change. |
| REQ-SLEWCAP-05 | Before live adoption, a before/after capture (cap off → on) must show the slip-induced direction-reversal / per-write step-irregularity in the loaded band drop with the cap enabled while the up-response (first-duty / ramp time) is unchanged, analyzed per `docs/response-evaluation-tuning-plan.md`, with no new `control_loop.authority_reasserted` events after startup. |

## 7. Data / schema deltas

- New field: `control_loop.rate_limit_max_elapsed_ms` (double, milliseconds;
  default NaN/absent → inert; identity preserved). No per-channel field, no runtime
  sidecar/status/CSV/manifest change, no `schema_version` bump. Existing configs and
  archives stay valid.
- Config impact: `config/control.release.json` (and `config/control.example.json` if
  it tracks the live cadence) gain the shipped cap value; `RateLimitSetpoint`
  signature gains one parameter (internal).

## 8. CLI / config / operator surface deltas

- One new config key (`control_loop.rate_limit_max_elapsed_ms`); no new CLI
  subcommand or flag.
- Doc updates at implementation (`AGENTS.md` §Change Checklist):
  `docs/CONTROL_PIPELINE_MATH.md` §8.1 (the amended identity + a §13 validation note),
  `docs/CONTROL_LOOP.md` (rate-limit behavior), and
  `docs/response-evaluation-tuning-plan.md` (the validation record). `README.md` only
  if a documented default changes.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/ratelimit-elapsed-cap-decision-2026-06-25.md`](../ratelimit-elapsed-cap-decision-2026-06-25.md) | Cap the rate-budget elapsed (vs fixed-per-tick step); loop-level config applied to all `RateLimitSetpoint` callers; default-off identity; shipped value ≈ nominal tick + margin. Shipped value + live evidence settled by the gate. | Current (direction); value + live evidence pending the gate |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-SLEWCAP-01 | T, R | `.\scripts\Test-LocalCI.ps1` C++ unit test of `RateLimitSetpoint`: capped elapsed clips the budget; cap unset reproduces the pre-feature result across a value matrix; review vs `docs/CONTROL_PIPELINE_MATH.md` §8.1. |
| REQ-SLEWCAP-02 | T, R | Config-contract + unit test asserting only the rate-budget elapsed is bounded (deadband/cooldown/cadence/channels/curves/step-cap/EMA unchanged; fall rate not raised); review of the diff. |
| REQ-SLEWCAP-03 | T | `RateLimitSetpoint` unit test: for `elapsed ≤ cap` the output equals the uncapped path bit-for-bit across the input matrix. |
| REQ-SLEWCAP-04 | T, R | Unit/integration test that the curve, PID, and low-band callers all pass the cap through; review that `docs/CONTROL_PIPELINE_MATH.md` §8.1 is amended in the same change. |
| REQ-SLEWCAP-05 | M | Live before/after capture analyzed with `svg-mb-control analyze ingest` + `analyze report` (plus the validated replay in `docs/intake-lead-grounding-2026-06-25.md` as supporting evidence): loaded-band reversal / step-irregularity drops with the cap on, up-response unchanged, no post-startup authority reasserts. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| The shipped `rate_limit_max_elapsed_ms` value | adoption | `300 ms` (`poll_tick_ms` + margin) candidate; settled by the §10 live gate within the acceptance band |
| Loop-level vs per-channel cap field | implementation | loop-level (slip is systemic); revisit only if a per-channel need appears |
| Whether to also clamp the low-band stage-boost RL elapsed or only the channel setpoint RL | implementation | clamp all callers uniformly (one shared `RateLimitSetpoint`); the low-band RL elapsed is the same systemic clock |

## 12. Measurement gate & dependencies

- **Measurement gate:** not crossed. `poll_tick_ms`, `write_cooldown_ms`,
  `deadband_pct`, and the channel set are unchanged; the cap bounds only the
  rate-budget elapsed and is inert at nominal cadence
  (`docs/MEASUREMENT_GATE.md`). Governed by `docs/response-evaluation-tuning-plan.md`.
- **Depends on:** `src/control/channel_evaluator.cpp` `RateLimitSetpoint` (shared by
  the curve law, the FEAT-0003 PID law, and the low-band stage boost) and the config
  loader. Independent of FEAT-0017/0024.
- **Build/test impact:** a C++ unit test for `RateLimitSetpoint` and a config-contract
  test; `docs/CONTROL_PIPELINE_MATH.md` §8.1/§13 and `docs/CONTROL_LOOP.md` updates.
  A `src/` behavior change → run `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2 — forensic timeline + validated replay, `docs/intake-lead-grounding-2026-06-25.md`).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [x] 3. Required design decision record(s) written and marked current (§9 — `docs/ratelimit-elapsed-cap-decision-2026-06-25.md` Current; shipped value + live evidence pending the gate).
- [x] 4. Concrete `REQ-SLEWCAP-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — `RateLimitSetpoint` unit test, config-contract test, response-evaluation live gate, contract review (§10), mirrored in `docs/TRACEABILITY.md`.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline (cap inert at nominal cadence; cadence/channels/deadband unchanged).
- [x] 7. Doctrine check: behavior claims grounded with file paths and validated evidence; the shipped value labeled a candidate; `must`/`should`/`is` per `CLAUDE.md`.

> Held at Draft: the shipped `rate_limit_max_elapsed_ms` value (§11) and the live
> before/after evidence (REQ-SLEWCAP-05) do not yet exist. Promote to Accepted only
> after the §10 unit tests land and a live capture confirms the reversal drop and the
> unchanged up-response.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-SLEWCAP-01 | | | |
| REQ-SLEWCAP-02 | | | |
| REQ-SLEWCAP-03 | | | |
| REQ-SLEWCAP-04 | | | |
| REQ-SLEWCAP-05 | | | |

**Spec vs. implementation deltas:** <record at implementation.>
