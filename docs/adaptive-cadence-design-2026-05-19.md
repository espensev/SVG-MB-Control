# Upward-Only Adaptive Cadence — Design (B1)

Status: **design only, not implemented.** No code in this change implements
this. It is the proposal requested in the code-quality pass follow-up.

## Problem

Fans look "jagged" when the control loop's sample/decide/write cadence is too
coarse relative to how fast the thermal situation is moving. Lowering
`poll_tick_ms` globally to fix this raises steady-state cost (hardware reads,
CSV rows) even when nothing is happening. The user constraint is explicit:

> Never raise the `poll_tick_ms` floor or reduce per-tick logged resolution.
> Cadence may only adapt **upward** — tighten under transients, never relax
> below the configured floor.

So the design is asymmetric: the configured `poll_tick_ms` is the **slowest**
the loop may ever run. Under thermal transients the loop may run **faster**
(shorter interval), adding resolution exactly when fans would otherwise look
jagged, and costs nothing in steady state.

## Invariant

Let `P = context.loop.poll_tick_ms` (the configured interval) and
`F = poll_tick_floor_ms` (a new config field: the fastest interval allowed).
Each iteration computes an effective interval `E` with:

```
F <= E <= P            (E never exceeds P → frequency never drops below 1000/P Hz)
E == P  in steady state (no transient → behaviour identical to today)
```

`F` exists only to bound the cost/limit imposed by the slowest physical input
(see "Hard floor on tick rate" below). `F` defaults to `P` (feature off) so the
change is inert until explicitly configured.

## Where it plugs in

Single integration point: `WaitForNextControlTick`
(`src/control_scheduler.cpp:135`). Today it computes
`next_tick_deadline = tick_started + milliseconds(poll_tick_ms)`. The change is
to pass an **effective interval** computed for the iteration just completed:

```
next_tick_deadline = tick_started + milliseconds(EffectiveTickIntervalMs(...))
```

Nothing else in the loop changes structurally. CSV rows, status writes, and
evidence files are already tick-driven, so a tighter cadence naturally produces
more rows during a transient — i.e. *more* logged resolution when it matters,
never less. Each row already carries `wall_clock` and `snapshot_age_ms`, and
the analyze tooling already tolerates irregular row spacing, so variable
spacing needs no downstream change (verify against `analyze` ingest before
implementing — listed as a risk below).

## Transient score

Compute a unitless `transient ∈ [0, 1]` each tick from signals already
available in the loop, take the max:

1. **Temperature slew.** Track CPU Tctl and GPU envelope from the previous
   tick; `slew = |ΔT| / Δt_ms`. Map `slew` through the existing `SmoothScale`
   helper between `cadence_slew_start_c_per_s` and `cadence_slew_full_c_per_s`.
   This is the primary jagged-fan driver.
2. **Low-band signal.** `context.low_band.signal` is already computed every
   tick (0..1). Reuse directly.
3. **Setpoint motion.** Max over channels of
   `|setpoint_pct_this_tick − setpoint_pct_last_tick| / Δt`, scaled. Captures
   the loop actively chasing a target (the exact moment jaggedness shows).

`transient = max(slew_score, low_band.signal, setpoint_motion_score)`.

`E = round(P − transient * (P − F))`. `transient = 0 → E = P` (unchanged);
`transient = 1 → E = F` (fastest).

Apply a one-sided rate limit on `E` *decreasing* fast (respond immediately to a
rising transient) but *increasing* slowly (decay back toward `P` over a few
seconds) so cadence does not oscillate and re-introduce jaggedness at the
boundary. Reuse `MoveTowardRateLimited` (already in `control_loop.cpp`).

## Hard floor on tick rate (why `F` is not 0)

The loop cannot tick faster than one full `SampleDirectRuntimeSnapshot`, whose
cost is dominated by the AMD PawnIO SMN reads + fan/EC reads (intrinsic,
in-process per project rules). `F` must be ≥ the measured worst-case
`loop_work_duration_ms` with headroom, else the loop perpetually overruns and
`loop_slip_ms` grows. Recommended: `F` defaults to `P`; when enabling, set `F`
no lower than ~2× observed steady `loop_work_duration_ms` from the status
file. The A1 change (one PCI-mutex acquire per sample instead of up to nine)
already lowered and de-jittered that work duration, which is what makes a
meaningfully lower `F` viable.

## Config surface (additive, default-inert)

New optional fields under the existing loop config:

| field | default | meaning |
|---|---|---|
| `poll_tick_floor_ms` | `= poll_tick_ms` | fastest allowed interval; `== poll_tick_ms` disables adaptation |
| `cadence_slew_start_c_per_s` | `0.5` | slew below this → no tightening |
| `cadence_slew_full_c_per_s` | `3.0` | slew at/above this → full tightening |
| `cadence_relax_per_s` | `(P−F)/3` | how fast E decays back up to P |

Validation: `poll_tick_floor_ms ≤ poll_tick_ms`; reject `0`; both ≥ a sane
minimum (e.g. 25 ms). Lives with the other loop-config validators in
`control_loop_config.cpp`.

## Observability / semantics change to document

`RuntimeControlLoopTimingState.loop_intended_interval_ms` is currently always
`poll_tick_ms`. With adaptive cadence it becomes the *effective* interval for
that tick. That is the correct meaning, but it is an observable schema-behaviour
change: anything asserting `loop_intended_interval_ms == poll_tick_ms` (tests,
dashboards, analyze) must be updated to treat it as variable and compare
against `[F, P]`. Add an explicit `cadence_transient` column/field so the
adaptation is auditable in the CSV/evidence.

## Test strategy (hermetic, sim-driven)

The existing sim hooks (`SVG_MB_CONTROL_SIM_AMD_TCTL_SEQUENCE_C`, low-band sim)
make this testable without hardware:

1. **Steady state**: flat temp sequence → assert every tick interval `== P`
   (feature inert / no behaviour change). Guards the invariant's lower bound.
2. **Transient ramp**: steep Tctl ramp sequence → assert observed tick
   intervals shorten, `min(interval) >= F`, and **no interval > P**. Guards
   the upward-only invariant.
3. **Decay**: ramp then flat → assert interval returns toward `P` gradually
   (rate-limited), not instantly.
4. **Disabled**: `poll_tick_floor_ms == poll_tick_ms` → behaviour byte-identical
   to pre-feature (regression guard).
5. Re-run `analyze ingest`/`report` on a variable-cadence run to confirm the
   offline tooling handles non-uniform row spacing.

## Risks

- **Overrun spiral** if `F` is set below sustainable `loop_work_duration_ms`.
  Mitigation: default `F = P`; document the "≥2× observed work" rule; consider
  auto-clamping `F` up if sustained `loop_overrun` is detected.
- **Schema/consumer breakage** from `loop_intended_interval_ms` becoming
  variable. Mitigation: audit analyze + dashboard + tests first; add the
  explicit `cadence_transient` field.
- **CSV volume** grows during sustained transients. Existing rotation/retention
  (`rotate_hours`, `retain_days`, `analyze prune`) already bound this; quantify
  worst-case (transient pinned at `F` for the rotation window) before shipping.
- **Oscillation** at the tightening boundary re-introducing jaggedness.
  Mitigation: asymmetric rate limit (fast tighten, slow relax) above.
- **Test coverage**: hermetic tests run sim mode; they validate the cadence
  *control logic* but not real-hardware `loop_work_duration_ms` headroom — that
  needs a real-hardware soak before trusting a low `F`.

## Recommended rollout

1. Land config fields + validation, default-inert (`F = P`), no behaviour
   change. Ships safely.
2. Land the `EffectiveTickIntervalMs` computation + `WaitForNextControlTick`
   integration + `cadence_transient` field + tests 1–5.
3. Real-hardware soak with `F` stepped down from `P` while watching
   `loop_overrun`/`loop_slip_ms`, to pick a safe `F` for this machine.
4. Only then document a recommended non-default `F`.

This keeps every step reversible and the invariant enforced by construction
(`E = clamp(E, F, P)`), so frequency/resolution can never regress below the
configured floor.
