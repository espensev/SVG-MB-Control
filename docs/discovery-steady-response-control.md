# Discovery - Steady Response Control

> Status, 2026-05-26: historical snapshot. This file documents the 2026-05-14
> steady-response pass against the **50 ms-era** controller profile and the
> pre-adoption floors. Every "50 ms write profile" reference (notably
> lines 37, 40, 102, 132) describes the prior shipped cadence; the current
> shipped profile is `poll_tick_ms = 250 / write_cooldown_ms = 250` with
> the adopted floors `15.5/22/60/56/31/20` (see
> `config\control.release.json`, `docs\NORMAL_RUNTIME_AIRFLOW_PROFILE.md`,
> and `docs\CONTROL_PIPELINE_MATH.md` §13.1). Use
> `docs\response-evaluation-tuning-plan.md` for current tuning guidance.

**Goal:** Evaluate the current usable fan response, sanity-check the control math,
and plan data-based adjustments without large changes or slower off-floor rise.
**Date:** 2026-05-14
**Status:** complete (superseded; see banner above)
**Recommended next:** none - standalone tuning plan

---

## Questions

1. What does the current live CSV show for temperature, setpoints, boost, and
   loop cost?
2. Are the old drop patterns still visible in the current response data?
3. Is the current control math coherent, or are any terms fighting each other?
4. Which tuning knobs should move first without slowing off-floor response?
5. Can this be simplified toward standard control terms?

---

## Findings

### Q1: Current live response

**Answer:** The current response is in the usable range. During the live run,
CPU/Tctl reached 86.625C. Radiator channels no longer sat near 50%: channel 1
peaked at 71.40%, channel 4 at 68.00%, and channel 5 at 65.86%. The slow boost
hit its configured cap on those lanes.

**Evidence:**
- `release/runtime/logs/archive/svg_mb_control_control-loop_20260514_033423.csv`
  analysis: 19,432 rows, CPU/Tctl avg 76.90C, max 86.625C.
- Same CSV: channel 1 setpoint max 71.40, boost max 22.00; channel 4 setpoint
  max 68.00, boost max 20.00; channel 5 setpoint max 65.86, boost max 20.00.
- Same CSV: loop interval avg 50.641 ms, min 50.003 ms, max 168.53 ms over the
  full file; current status showed no overrun and process CPU around 0.29%.

**Implications:**
- The new slow term is doing the steady-pressure job.
- The current high-temp setpoints are now materially higher, without changing
  the fast 50 ms write cadence.

### Q2: Drop patterns

**Answer:** The old large staircase is reduced, but some downward candidate
steps remain on channels 1 and 4. Channel 5 is already clean by the >0.35%
criterion.

**Evidence:**
- CSV analysis, drops >0.35% per logged candidate step: channel 1 = 60,
  channel 4 = 52, channel 5 = 0.
- Worst candidate drops: channel 1 = -0.417%, channel 4 = -0.383%,
  channel 5 = -0.293%.
- Examples occur around 82.5-83.0C with boost saturated, so they are mainly
  filtered base-demand drops and deadband/write quantization, not pressure
  collapse.

**Implications:**
- This is not a large control failure. If audible, the next adjustment should
  be small: reduce channel 1/4 effective downward step size or fall filter,
  not rebuild the controller.

### Q3: Control math sanity

**Answer:** The current structure is coherent for a nonlinear thermal system:
gain-scheduled feed-forward curve, asymmetric first-order smoothing, bounded
slow integral trim, final slew limiter, then deadband/cooldown quantization.

**Evidence:**
- `src/control_loop.cpp:334` - `ApplyDemandSmoothing` applies asymmetric
  demand filtering and bounded fall behavior.
- `src/control_loop.cpp:383` - `UpdateThermalPressureBoost` accumulates a
  bounded slow boost from sustained heat and clamps it for anti-windup.
- `src/control_loop.cpp:1160` - pressure boost is added after base smoothing
  and before final rate limiting.
- `src/control_loop.cpp:1167` - final `RateLimitSetpoint` is still the last
  movement limiter before write policy/deadband.

**Implications:**
- PID is not the right next step. The existing model is closer to a practical
  feed-forward plus integral trim controller.
- The main caveat is naming/shape: `thermal_pressure_*` is a bounded integral
  trim, while `decay_latch_*` is really a fall slew cap.

### Q4: Adjustment order

**Answer:** Keep rise behavior as-is. Tune only steady and falling behavior
from measured runs.

**Evidence:**
- Time from pressure threshold to 60%: channel 1 = 7.4 s, channel 4 = 8.5 s,
  channel 5 = 9.8 s.
- After falling below 82C, boost released to zero in about 39.8-43.8 s and
  lanes returned to floor in about 37.8-41.8 s.

**Implications:**
- Do not reduce rise alpha or rise rate; off-floor response is already fast
  enough.
- If high-temp steady duty is still low, adjust max boost or curve around
  86-90C.
- If release is too slow, raise pressure fall rate from 0.5%/s toward
  0.75-1.0%/s.
- If downward steps are audible, lower channel 1/4 deadband or fall alpha
  slightly before changing the whole control model.

### Q5: Standardization/simplification

**Answer:** A small follow-up refactor can make the controller more standard
without changing behavior: express smoothing as time constants, rename latch
fields as slew limits, and describe pressure as a bounded integral trim.

**Evidence:**
- Existing EMA alphas are tick-dependent; with a 50 ms tick, alpha 0.20 is
  about a 0.22 s time constant, alpha 0.035 is about 1.4 s.
- Existing pressure boost has anti-windup clamp and separate fall decay,
  matching a bounded integral trim more than a custom latch.

**Implications:**
- The next code cleanup should be compatibility-preserving: add
  `*_time_constant_ms` aliases and compute `alpha = 1 - exp(-dt / tau)`.
- Keep the current config working while documenting the standard names.

---

## Cross-Cutting Analysis

### Constraints

- The 50 ms write profile is part of the working response and should stay.
- The off-floor rise must not be made slower.
- Channel 6 remains out of live control.
- Tuning should stay channel-specific; radiator channels 1, 4, and 5 do not
  behave identically.

### Risks

| Risk | Likelihood | Impact | Notes |
|------|------------|--------|-------|
| Over-tuning steady boost | Medium | Medium | Higher max boost can raise noise during long heat-soak. |
| Slowing off-floor response | Low | High | Avoid changing rise alpha/rise rate unless data proves it. |
| Too-slow cooldown release | Medium | Medium | Pressure fall rate controls this; adjust directly if needed. |
| More writes from lower deadband | Medium | Low | Current process CPU is low, so there is headroom. |

### Open Questions

- Whether the remaining 0.35-0.42% downward candidate steps are audible in the
  room. Data shows them; perception decides if they need tuning.

---

## Recommendation

Do not make a large control change now. Continue with the current controller and
collect one or two comparable heat runs. Tune in this order:

1. If radiator duty still feels low near 86-90C, add 3-5% max boost to channels
   4/5 first, then channel 1 only if needed.
2. If release is still too slow after load ends, raise radiator
   `thermal_pressure_fall_pct_per_sec` from 0.5 to 0.75.
3. If downward steps remain audible while hot, add channel-specific deadband
   overrides of 0.25 on channels 1 and 4, or reduce their fall alpha from 0.035
   to about 0.025.
4. In a later cleanup pass, preserve behavior but rename/express the math as
   standard first-order filters, slew limits, and bounded integral trim.
