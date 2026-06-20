# Selectable Control-Law Profile Decision - 2026-06-03

Status: Current for the FEAT-0003 control-law seam, PID structure, controller
state ownership, shared output conditioning, and measurement-gate posture.
Updated 2026-06-20: FEAT-0003 is reopened as a restart-selected later phase after
FEAT-0023, not as an in-process hot-swap. The original in-process swap choices
(D5/D7) are historical for the current scope and are superseded by
`docs/multiprofile-restart-switch-decision-2026-06-20.md` D-MPROFILE-6. This
record does not authorize implementation by itself.

PID remains the worked **example of the range** of control laws the per-channel
seam must be able to host — a feedback law as different from the current
feed-forward curve as possible — so the abstraction is designed for that breadth
rather than curve-specific. Design decision record for
`docs/features/FEAT-0003-selectable-profile-hot-swap.md`.

**Companion to:** `docs/features/FEAT-0003-selectable-profile-hot-swap.md`,
`docs/features/FEAT-0001-hot-swap-write-policy.md`, `docs/CONTROL_LOOP.md`,
`docs/CONTROL_PIPELINE_MATH.md`, `docs/MEASUREMENT_GATE.md`,
`docs/RUNTIME_HOME.md`.

PID = proportional-integral-derivative. P / PI / PD are its degenerate forms
with one or more gains set to zero.

## Problem

The per-channel control law is fixed in code. `EvaluateChannel`
(`src/control/channel_evaluator.cpp:440-474`) is the only control law: a
feed-forward temperature→duty curve plus a CPU-override curve, demand smoothing,
four boost overlays, a low-band residual, and a per-channel rate limiter. The
tick body calls it directly (`src/control/tick_runner.cpp:241-243`).

The operator wants a resolved profile to select a different control **law** — for
example a PID controller (or its P / PI / PD forms) that regulates a measured
temperature to a target temperature — not only different tuning numbers for the
existing law. Two facts make this more than a config-key toggle:

1. **The current law is feed-forward; PID is feedback.** Today
   `duty = curve(temp) + overlays`. PID computes `error = temp − target` and
   `duty = bias + Kp·error + Ki·∫error + Kd·d(error)/dt`. PID needs state the
   current law has no slot for (an integral accumulator; a previous error/temp
   and timestamp for the derivative).
2. **Config and per-channel dynamic state are welded.** `ChannelState`
   (`src/control/control_runtime_context.h:17-84`) holds the channel `config`
   next to the law's dynamic state (`boosts[]`, `smoothed_demand_pct`,
   `last_issued_pct`, `low_band_*`). The constructor copies each
   `loop.channels[i]` into `ChannelState.config`
   (`src/control/control_runtime_context.cpp:17-21`). There is no defined seam
   for which state carries over and which resets when the law or profile changes.

This decision settles how a swappable control law is structured, how PID is
shaped, what happens to dynamic state on a swap, and how a live PID switch stays
inside `docs/MEASUREMENT_GATE.md`.

## D1 — Control-law seam (the abstraction)

**Decision: introduce `IChannelController`, a per-channel control-law interface,
and make the current law `CurveOverlayController`.**

The interface exposes the existing result type and the existing inputs, so the
tick body changes at one call site only:

```text
class IChannelController {
  virtual ChannelEvaluation Evaluate(ChannelState& channel,
                                     const ControlLoopConfig& loop,
                                     const TempInputs& temp_inputs,
                                     const RuntimeSnapshotIndex& runtime_index,
                                     std::chrono::steady_clock::time_point now) = 0;
  virtual void Reset() = 0;            // clears the law's dynamic state
  virtual std::string_view Kind() const = 0;   // "curve_overlay" | "pid"
  virtual ~IChannelController() = default;
};
```

Rationale:
- `EvaluateChannel` is already an input→output function with no I/O
  (`channel_evaluator.cpp:440-474`): it reads `ChannelState` + `loop` + inputs,
  mutates dynamic state on `ChannelState`, and returns a `ChannelEvaluation`. It
  is a control law behind a single call site (`tick_runner.cpp:241`). Wrapping it
  is a move, not a rewrite.
- A second law (PID) added by branching inside `EvaluateChannel` would put two
  unrelated laws in one translation unit. An interface keeps each law in its own
  module and keeps the tick body law-agnostic.

Rejected alternative — a config-flag branch inside `EvaluateChannel`: it grows a
single function to hold every law and shares no seam with the swap mechanism.

## D2 — Where control-law dynamic state lives

**Selected direction (maintainer, 2026-06-03; for a demonstration build): full
decouple in one pass.** Move the curve law's dynamic state out of `ChannelState`
into `CurveOverlayController`, add `PidController` (which owns its own
integral/derivative state), and slim `ChannelState` to law-agnostic fields only —
in one change.

- `CurveOverlayController` owns `boosts[]`, `smoothed_demand_pct`, and the
  `low_band_*` accumulators; `ChannelState` keeps only law-agnostic fields
  (baseline, `write_active`, `hold_deadline`, breaker, `last_issued_pct`,
  `last_write_time`, sensor-failure counters, reporting fields).
- `PidController` owns its integral accumulator and previous error/temp/time.
- `Reset()` becomes a controller rebuild; no `ChannelState` field-zeroing path is
  needed.

Clarification (2026-06-06): `last_raw_demand_pct` is not law-agnostic merely
because it is reported. It is written by the curve law and published as
`feedforward_pct`, so a future decouple should treat it as curve-controller-owned
or publish it through a kind-aware/nullable reporting field rather than keeping it
as a generic `ChannelState` reporting field.

Rationale: removes the config+state welding (Problem #2) in the same change that
introduces the seam, so there is one end state rather than an interim with welded
curve state. Cost: the larger first change must reproduce today's
`EvaluateChannel` output exactly (FEAT-0003 REQ-PROFILE-01), so the
output-equivalence test over representative inputs is the gate that this move did
not alter the curve law.

## D3 — PID structure

PID covers P, PI, PD, and PID with one class by gain selection: a zero gain
disables that term, so pure-P is `Ki = Kd = 0`, PI is `Kd = 0`, PD is `Ki = 0`.
No class per form. An optional `form` config label is informational; the gains
are authoritative.

Sub-decision D3a is selected below; D3b and D3c carry recorded leans:

- **D3a — bias / feed-forward (selected 2026-06-03): selectable.** A
  `pid.feedforward` field selects the form per channel: `"curve"` →
  `output = curve(temp) + PID(error)` (PID corrects the existing feed-forward
  curve, which stays as the shape/floor); `"fixed"` → `output = bias_pct +
  PID(error)` (a standalone PID around a constant resting duty). Both forms share
  one `PidController`; `feedforward` selects the baseline the PID terms are added
  to. Cost: both code paths are validated (the larger test matrix noted in
  FEAT-0003 §11).
- **D3b — derivative variant.** Derivative-on-error (`Kd·d(error)/dt`) **vs.**
  derivative-on-measurement (`−Kd·d(temp)/dt`). Derivative-on-measurement avoids
  a setpoint-change "derivative kick" when `target_c` is swapped live, which this
  feature can do. Lean: **derivative-on-measurement.**
- **D3c — integral anti-windup.** Clamp the integral accumulator to
  `[integral_min, integral_max]` and stop integrating while the output is
  saturated at `min_duty_pct` or `100`. Lean: **clamp + conditional integration
  (back-calculation deferred).**

Shared with the curve law (no PID-specific copy): primary-temperature selection
including `temp_blend` and the source-aware guard (`SelectPrimaryCurveInput`,
`channel_evaluator.cpp:198-237`); sensor-safe mode
(`channel_evaluator.cpp:248-264`); the `[min_duty_pct, 100]` clamp.

## D4 — Shared output conditioning vs. curve-law-specific dynamics

**Decision:** the following are law-agnostic and apply to every controller via
the shared output path (`channel_write.*`, `TryApplyChannelSetpoint`,
`channel_write.h:55`): clamp to `[min_duty_pct, 100]`, sensor-safe mode,
deadband, write cooldown, control-hold, circuit breaker, baseline
capture/restore, and the write gate.

**Open (lean recorded):** the current curve-law output dynamics —
- the rise/fall/max-step **rate limiter** (`channel_evaluator.cpp:37-70`): lean
  **shared** as a safety slew clamp (a mis-tuned PID should not be able to step
  the duty arbitrarily fast), applied after the law produces a setpoint;
- the **EMA demand smoothing + decay latch** (`channel_evaluator.cpp:72-117`):
  lean **curve-law-specific** — a PID has its own integral/derivative dynamics
  and should not also be EMA-smoothed.

## D5 — Dynamic-state carry-over vs. reset on swap (historical; restart scope uses fresh worker state)

**Decision: reset by default.** On a profile swap, each channel's control-law
dynamic state is reset (PID integral = 0 and derivative memory cleared; curve
smoothing/boost/low-band-stage state cleared). Carry-over is opt-in and out of
the first slice.

2026-06-20 update: current FEAT-0003 no longer swaps laws in process. The active
meaning is simpler: controller state is scoped to one worker lifetime, and a
FEAT-0023 profile switch restarts the worker into fresh controller instances.

Rationale: a law change makes carried state meaningless (a curve `smoothed_demand`
has no meaning to a PID integral). Even a same-law swap is more predictable when
it starts from a defined state. The shared output stage still rate-limits the
first post-swap setpoint, so reset does not produce an unbounded step.

## D6 — Live authorization and the measurement gate

**Selected direction (maintainer, 2026-06-03; revised 2026-06-06): shadow/dry-run
default; live PID via an explicit per-channel opt-in gated on evidence and a slew
bound.** A PID channel with `pid.allow_live` absent or false computes and logs its
setpoint but suppresses the write (shadow/dry-run), so PID can be exercised and
evidenced without moving the baseline. `pid.allow_live: true` authorizes a live PID
write only when **both** hold: (a) the channel has the characterization evidence
`docs/MEASUREMENT_GATE.md` requires — a shadow-log comparison of the PID trajectory
against the curve baseline for that channel is accepted as that evidence; and
(b) the channel sets a non-NaN, positive slew cap (`max_setpoint_step_pct` and/or
the rise/fall rate fields). Both are enforced as a config-load precondition: with
either missing, `allow_live: true` is rejected at load and the channel stays in
shadow/dry-run.

A live `allow_live` crossing must still emit a `control_loop.profile_applied` event
naming the channel and the law, so the baseline move is recorded as well as
evidenced. The shared output guards (clamp to `[min_duty_pct, 100]`, the slew cap,
sensor-safe mode, the circuit breaker) remain the safety floor regardless.

**Revised 2026-06-06 (why).** The original 2026-06-03 form authorized `allow_live`
*immediately, without first requiring the characterization evidence*, on the ground
that the crossing was explicit and recorded. A 2026-06-06 review
(`docs/archive/profile-hot-swap-allow-live-decision-2026-06-06.md`; the
measurement-gate section of
`docs/archive/modular-profile-hotswap-discussion-2026-06-06.md`) found that
`docs/MEASUREMENT_GATE.md` Exit Criteria are measurements, not authorizations, so a
recorded crossing is *consent, not evidence*; and that the slew cap the floor
relies on defaults to NaN/off in code (`src/control/control_loop.h:29-31`;
`src/control/channel_evaluator.cpp:55-57`), so the floor was not guaranteed
present. The revised form requires both the evidence (lever B1) and the slew-cap
precondition (lever B2). B1 and B2 are complementary, not redundant: a shadow-log
comparison is the best pre-live evidence available, but it cannot exhibit the
closed-loop modes (limit cycle, overshoot, integral windup) that appear only once
PID drives the loop it reads, so B2's slew cap is the standing bound on exactly what
B1's evidence cannot catch.

## D7 — Apply order (historical; superseded by FEAT-0023 restart switching)

2026-06-20 update: current FEAT-0003 does not add a runtime-home request consumed
by the tick runner and does not swap controller instances mid-worker. FEAT-0023
owns the profile-switch request, validation, supervised restart, and revert
behavior. The old in-process apply order is intentionally not retained here; it
is no longer the current design.

## Consequences

- The tick body changes at one call site; both laws live in their own modules.
- `ChannelState` is slimmed to law-agnostic fields; the curve law's dynamic state
  moves into `CurveOverlayController` and the PID state lives in `PidController`
  (D2).
- `CONTROL_PIPELINE_MATH.md` becomes the identity reference for the curve+overlay
  law (`CurveOverlayController`) and its values stay byte-identical; the PID law
  gets its own identity reference (FEAT-0003 REQ-PROFILE-10).
- Status and CSV gain an additive per-channel controller-kind field so behavior
  can be attributed to the law that produced it (REQ-PROFILE-08).
- Construction-time consumers (the CSV logger identity block,
  `control_loop.cpp:66-77`; the `FanWriter`) are rebuilt with the worker under
  the restart-selected scope; a live `poll_tick_ms` change remains out of scope.
- No control-identity change to the curve law; `docs/CONTROL_PIPELINE_MATH.md`
  values are preserved, scope narrowed.

## Selected directions (maintainer, 2026-06-03)

1. **D3a:** selectable — `pid.feedforward: "curve" | "fixed"`.
2. **D2:** full decouple in one pass — the controller owns its dynamic state and
   `ChannelState` is slimmed to law-agnostic fields.
3. **D6 (revised 2026-06-06):** shadow/dry-run default; live PID via an explicit
   `pid.allow_live` per-channel opt-in that requires both characterization evidence
   (shadow-log comparison accepted) and a non-NaN slew cap, enforced at config load.

These remain the current directional choices for the restart-selected FEAT-0003
Draft. They are not implementation authorization by themselves; FEAT-0003 is
sequenced after FEAT-0023. PID stands in as the worked example of how different a
selectable control law may be from the current feed-forward curve.

## Verification

- `.\scripts\Test-LocalCI.ps1`: `CurveOverlayController` output-equivalence vs.
  the current `EvaluateChannel`; one `PidController` exercised as P/PI/PD/PID by
  gain selection; a swap resets dynamic state; a validation failure on swap
  retains the running profile and emits a rejection; shared output conditioning
  (sensor-safe, deadband, cooldown, breaker, clamp) identical across laws.
- Code review vs. `docs/CONTROL_LOOP.md`, `docs/CONTROL_PIPELINE_MATH.md`, and
  this decision: single control-law call site; curve identity preserved; PID
  identity documented.
- Runtime evidence that a FEAT-0023 profile switch restarts the worker into the
  requested law, plus PID shadow/dry-run behavior when exercised live (respecting
  `AGENTS.md` §Live Runtime Safety).

## Reconsideration (2026-06-06) — resolved

A 2026-06-06 review questioned whether **D6**'s original
`allow_live`-without-evidence posture satisfied `docs/MEASUREMENT_GATE.md` (read as
an evidence gate, not a consent gate), and noted that the slew-cap floor D6 relies
on defaults to NaN/off in code (`src/control/control_loop.h:29-31`;
`src/control/channel_evaluator.cpp:55-57`). **Resolved 2026-06-06:** the maintainer
selected B1+B2 — `allow_live` now requires both the characterization evidence and a
non-NaN slew cap (the D6 text above is the revised form). The options considered are
in `docs/archive/profile-hot-swap-allow-live-decision-2026-06-06.md`.
