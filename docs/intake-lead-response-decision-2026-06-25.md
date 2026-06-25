# Intake-lead fan response under load — decision (2026-06-25)

**Status:** Current (direction). Candidate magnitudes are labeled and are settled
by the response-evaluation Pass-3 validation before adoption.
**Owns the direction for:** `docs/features/FEAT-0024-intake-lead-under-load.md`
(`REQ-INLEAD-*`).
**Companion to:** `docs/features/FEAT-0017-faster-fan-reaction-under-load.md`
(reuses its rate-limit identity), `docs/COOLING_STRATEGY.md`,
`docs/response-evaluation-tuning-plan.md`, `docs/CONTROL_PIPELINE_MATH.md`,
`docs/control-latency-reduction-design-2026-06-18.md`.

> Dated design-decision record per `docs/features/README.md` §3 promotion gate 3.
> This settles the **direction** (which lanes, which mechanism, what is out of
> scope). It does not authorize implementation on its own; promotion of FEAT-0024
> still requires the Pass-3 validation in that spec's §10/§12.

## 1. Context (operator report)

The operator's standing request, captured 2026-06-25: the controlled fans should
put **more focus on which lanes lead** — the **intake lanes should supply airflow
first** as temperatures climb, and may surge hard ("even overshoot") on a rapid
climb. The operator separately stated that idle / non-critical-speed resonance is
**not** a concern to act on right now ("idle's basically fine"), so this work is
scoped to the **climb** regime only.

Two supporting observations from live evidence this session:

- At a clean idle window (boosts = 0, 5 min), the exhaust lanes sit steady at
  their floors while the **intake `cpu_override` baselines** track the CPU Tctl
  composite (Tctl idle wander 54.9–67 °C); the 200 mm front pair ch2/ch3 sit
  ~30 rpm apart. These were investigated as the resonance/"not tight" candidates
  but were **deprioritized by the operator**.
- A read-only control-pipeline analysis (2026-06-25) established that on a rising
  transient the intake lanes do **not** uniformly lead: the 200 mm intakes
  ch2/ch3 lead the exhausts on slew, but **ch4 (front-radiator intake) is the
  single slowest-ramping channel in the system** (`rise_rate_pct_per_min = 60`,
  `demand_smoothing_rise_alpha = 0.008`), and the intake `cpu_override` curves are
  comparatively flat through the mid band, so the intakes are parked near their
  floor under load while the exhaust lanes carry the dynamic range.

## 2. Decisions

### D-INLEAD-1 — The lead lanes are the intake lanes 2, 3, 4
The intake-lead retune targets channels `2`, `3`, `4` (the two PA602 200 mm front
intakes and the front-radiator Noctua intake). This **re-targets** the FEAT-0017
joint rate-limit mechanism, which currently leans toward the radiator exhaust
lanes `1`/`4`/`5` and explicitly leaves the 200 mm pair unchanged
(FEAT-0017 §3, §11). ch4 receives the largest rate raise because it is both an
intake and the slowest-ramping channel, and it feeds the AIO radiator directly.

### D-INLEAD-2 — Surge-and-hold (config-only), not a literal overshoot term
The intakes engage **earlier** (lower `gpu_airflow_start_c` than the exhausts),
ramp **harder** (raised per-lane rate limiter + raised intake boost ceilings +
steeper intake `cpu_override` mid-band), and **hold** the resulting airflow while
the temperature stays up. This is achieved with **config values only**.

A genuine rate-of-rise (dT/dt) "overshoot" term was considered and **declined**:

- The shipped curve/overlay control law has no proportional or derivative term;
  its boosts are saturating upward **level** integrators and the rate limiter
  never steps past its target, so it **structurally cannot** produce a dynamic
  overshoot above the settled target (`src/control/channel_evaluator.cpp`,
  `src/control/boost_stage.cpp`; `docs/CONTROL_PIPELINE_MATH.md` invariants).
  A real overshoot term would require new C++ (a dT/dt term in
  `ComputeFinalSetpoint` plus per-channel previous-temperature state), a
  `CONTROL_PIPELINE_MATH.md` identity change, its own decision record, and a live
  measurement gate.
- A level-based surge **holds** (no recover-down), whereas an overshoot term
  rises above target then settles back; that whoosh-and-recover is itself an
  audible variation the operator dislikes elsewhere. Surge-and-hold delivers the
  intake-lead intent without it.
- The prior power-feed-forward work is consistent with this: the RAPL/power
  control input was a no-go (2026-06-15) because package watts gave 0 s onset lead
  over the free `system_cpu_busy_pct` signal, and the anticipation follow-up
  recorded that a temperature-rate (`dTctl/dt`) term, not power, is the mechanism
  to use **if** anticipation is ever pursued — and that the absolute exploitable
  lead on this machine is thin. That keeps a future dT/dt term as a separate,
  evidence-gated option, not part of this config retune.

### D-INLEAD-3 — Idle / low-band is out of scope (unchanged)
No intake curve knot at or below `72 C` changes, and no idle re-spacing is
performed. The lowered `gpu_airflow_start_c = 58 C` is above the idle GPU envelope
(~46 C) so it does not fire at idle; the steeper `cpu_override` only applies above
the idle Tctl wander band. The front-200 mm `>= 4%` spacing and the soft-floor
contracts are preserved unchanged.

### D-INLEAD-4 — Asymmetric: spin-down is not made faster
No falling-direction value (`fall_rate_pct_per_min`,
`demand_smoothing_fall_alpha`, `decay_latch_pct_per_min`) is raised on any lane.
The deliberate slow, quiet spin-down (`docs/COOLING_STRATEGY.md`) is preserved.

### D-INLEAD-5 — Radiator CPU authority ordering is preserved
The intake high-end CPU response (`>= 88 C` `cpu_override` knots) stays **below**
the channel `1`/`5` radiator-exhaust emergency knees, keeping the
highest-authority high-CPU response on the radiator exhausts per
`docs/response-evaluation-tuning-plan.md` ("CPU Override"). The intake steepening
acts on the **onset** of the climb (mid band), not the ceiling.

## 3. Candidate magnitudes (settled by Pass-3)

Per-lane, intake lanes only; everything else unchanged. These are **candidates**;
the Pass-3 combined-load capture confirms the intake-lead margin and tunes the
exact values within the acceptance band.

| Field | ch2 | ch3 | ch4 | Note |
|---|---|---|---|---|
| `rise_rate_pct_per_min` | 90 → **125** | 90 → **125** | 60 → **120** | both knobs raised together (FEAT-0017 identity); ch4 the largest |
| `max_setpoint_step_pct` | 0.7 → **0.95** | 0.7 → **0.95** | 0.6 → **0.95** | raised with the rate so neither becomes the sole binding cap |
| `gpu_airflow_start_c` | 62 → **58** | 62 → **58** | 64 → **58** | intakes engage ~6 C of GPU rise before the exhausts (which stay 64) |
| `gpu_airflow_max_boost_pct` | 8 → **12** | 8 → **12** | 5 → **10** | intakes surge harder than the exhausts (4–5) |
| `cpu_override_curve` 72–86 C | steeper | steeper | steeper (most) | onset of the climb; `<= 72 C` knots unchanged; `>= 88 C` knots stay below ch1/ch5 |
| `demand_smoothing_rise_alpha` | — | — | 0.008 → **0.014** (optional) | shorten ch4's approach tail; fall alpha unchanged |

Effective rise ceiling `min(rise/60, step*1000/250)` rises on every retuned lane:
ch2/ch3 `1.5 → 2.08 %/s`, ch4 `1.0 → 2.0 %/s`. The exhaust lanes stay at
`1.25 %/s`, so the intakes ramp ~65 % faster.

## 4. Validation & rollout

- **Pass-1 (idle hold):** confirm idle per-channel setpoint / RPM are unchanged
  versus the pre-change baseline (the change must not touch idle).
- **Pass-3 (combined CPU + GPU load):** confirm the intake lanes reach
  first-duty-increase and complete their ramp **sooner than the exhaust lanes**
  on a climb, with CPU Tctl and GPU memory percentiles inside the
  `docs/response-evaluation-tuning-plan.md` acceptance band and no new
  `control_loop.authority_reasserted` events after startup.
- **Deploy:** live-deploy the validated config and verify before/after, with a
  clean rollback to the prior `release/control.json`, under explicit live-runtime
  authorization (`AGENTS.md` §Live Runtime Safety).

## 5. Evidence basis

- Live idle window 2026-06-25 (boosts = 0): exhausts steady at floors; intakes
  track Tctl; ch2/ch3 ~640/670 rpm.
- Control-pipeline read-only analysis 2026-06-25 (curve/overlay law has no
  overshoot term; ch4 is the slowest channel; intake `cpu_override` flat mid-band;
  `gpu_airflow` is the only intake-favoring boost).
- `docs/features/FEAT-0017-faster-fan-reaction-under-load.md` (rate-limit
  identity, asymmetry rule, Pass-3 governance) and
  `docs/control-latency-reduction-design-2026-06-18.md` (D-REACT-1).
- `docs/archive/cpu-power-feedforward-plan-2026-06-10.md` (power no-go) and
  `docs/archive/cpu-transient-power-anticipation-plan-2026-06-15.md`
  (`dTctl/dt` is the mechanism if anticipation is pursued; thin absolute lead).
