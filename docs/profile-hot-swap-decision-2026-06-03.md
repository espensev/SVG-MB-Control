# Selectable Control-Law Profile Hot-Swap Decision - 2026-06-03

Status: design-capture only. Per the maintainer (2026-06-03), this feature is not
scheduled and is not believed to be a net benefit; PID is recorded here as a
worked **example of the range** of control laws the per-channel seam must be able
to host — a feedback law as different from the current feed-forward curve as
possible — so the abstraction is designed for that breadth rather than
curve-specific. Any implementation would be a demonstration. The directional
choices below (D2, D3a, D6) record what such a demonstration would do if
undertaken; they are not a commitment to build. Design decision record for
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

The operator wants to select a different control **law** at runtime — for
example a PID controller (or its P / PI / PD forms) that regulates a measured
temperature to a target temperature — not only different tuning numbers for the
existing law. Two facts make this more than a config swap:

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

## D5 — Dynamic-state carry-over vs. reset on swap

**Decision: reset by default.** On a profile swap, each channel's control-law
dynamic state is reset (PID integral = 0 and derivative memory cleared; curve
smoothing/boost/low-band-stage state cleared). Carry-over is opt-in and out of
the first slice.

Rationale: a law change makes carried state meaningless (a curve `smoothed_demand`
has no meaning to a PID integral). Even a same-law swap is more predictable when
it starts from a defined state. The shared output stage still rate-limits the
first post-swap setpoint, so reset does not produce an unbounded step.

## D6 — Live authorization and the measurement gate

**Selected direction (maintainer, 2026-06-03): shadow/dry-run default; live PID
via an explicit per-channel opt-in.** A PID channel with `pid.allow_live` absent
or false computes and logs its setpoint but suppresses the write (shadow/dry-run),
so PID can be exercised and evidenced without moving the baseline.
`pid.allow_live: true` authorizes live PID writes immediately, without first
requiring the characterization evidence.

This stays inside the `docs/MEASUREMENT_GATE.md` invariant because the crossing is
**explicit and recorded**, not silent: `allow_live` is an operator opt-in in
config and must emit a `control_loop.profile_applied` event naming the channel and
the law, so the baseline move is auditable. The shared output guards (clamp to
`[min_duty_pct, 100]`, the slew cap, sensor-safe mode, the circuit breaker) remain
the safety floor regardless. The maintainer accepts that live PID under
`allow_live` runs off the characterized curve baseline before evidence exists.

## D7 — Apply order (normative for implementation)

A profile change is delivered as a runtime-home request file consumed once per
tick, mirroring the breaker-reset pattern (`TakeRuntimeBreakerResetRequest`,
`src/runtime/runtime_lifecycle.cpp:55-103`; intake at `tick_runner.cpp:195`). On
a valid request:

1. **Validate.** Parse and validate the candidate profile via
   `LoadControlLoopConfig` (`control_loop_config.cpp:507-568`) plus per-law
   validation. On failure keep the running profile, emit
   `control_loop.profile_rejected`, and stop — no channel state is touched.
2. **Channel-set delta (if any).** For the first slice the candidate's channel
   set must equal the running set or the request is rejected. When FEAT-0001 is
   implemented, dropped channels restore baseline and clear `write_active` via
   `RestoreSavedState` / `HandleExpiredHoldRestore` (`channel_write.h:38-48`);
   added channels capture baseline fresh; survivors match by `channel` id.
3. **Build + reset + swap.** Build each channel's new controller, reset its
   dynamic state (D5), then swap `context.loop`, each
   `context.channels[i].config`, and each controller at the tick boundary. Emit
   `control_loop.profile_applied` with the per-channel law/parameter delta. The
   next tick evaluates with the new law.

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
  `control_loop.cpp:66-77`; the `FanWriter`) are unaffected by a law/param swap
  on a fixed channel set; a live `poll_tick_ms` change remains out of scope, as
  in FEAT-0001.
- No control-identity change to the curve law; `docs/CONTROL_PIPELINE_MATH.md`
  values are preserved, scope narrowed.

## Selected directions (maintainer, 2026-06-03)

1. **D3a:** selectable — `pid.feedforward: "curve" | "fixed"`.
2. **D2:** full decouple in one pass — the controller owns its dynamic state and
   `ChannelState` is slimmed to law-agnostic fields.
3. **D6:** shadow/dry-run default; live PID via an explicit `pid.allow_live`
   per-channel opt-in.

These record what a demonstration build would do. They are not a commitment to
build; per the status above the feature is not scheduled and is not believed to be
a net benefit. PID stands in as the worked example of how different a swappable
control law may be from the current feed-forward curve.

## Verification

- `.\scripts\Test-LocalCI.ps1`: `CurveOverlayController` output-equivalence vs.
  the current `EvaluateChannel`; one `PidController` exercised as P/PI/PD/PID by
  gain selection; a swap resets dynamic state; a validation failure on swap
  retains the running profile and emits a rejection; shared output conditioning
  (sensor-safe, deadband, cooldown, breaker, clamp) identical across laws.
- Code review vs. `docs/CONTROL_LOOP.md`, `docs/CONTROL_PIPELINE_MATH.md`, and
  this decision: single control-law call site; curve identity preserved; PID
  identity documented.
- Runtime evidence for `control_loop.profile_*` events and PID shadow/dry-run
  behavior when exercised live (respecting `AGENTS.md` §Live Runtime Safety).
