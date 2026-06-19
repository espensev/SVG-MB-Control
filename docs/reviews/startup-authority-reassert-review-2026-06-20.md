# Startup Authority-Reassert Review - 2026-06-20

## Verdict

The startup authority-reassert burst across channels `0-5` is intentional under
the current control contract. It is the normal continuous-hold startup path:
sample direct fan telemetry, capture a baseline for each configured channel,
compute the current setpoint, then write any channel whose observed fan state is
not already under Control authority or whose setpoint is outside the deadband.

I do not recommend changing behavior from this review alone. The behavior is
bounded by the shipped channel set, per-channel deadband/cooldown gates, and
per-channel setpoint step caps. There is one residual: the implementation does
not globally stagger first-tick writes across channels, so a startup where all
six controlled fans need authority will issue up to six `ApplyDuty` calls in one
tick. That is consistent with today's docs and measurement gate, but it is not
locked by a focused regression test.

## Findings

- Sound for current release profile: 38 recent release startup rows all reasserted
  channels `0-5` at tick 1, and the largest first-tick duty movement was
  `0.805%`.
- Not sound as a universal claim: the older `manual-90-smoke` run at
  `cpu_tctl_c=81.500 C` jumped to `90-100%`, proving this depends on the active
  ramp/step-cap config.
- Control reasoning supports reassertion as authority recovery when fan mode is
  not controller-owned; it does not require all channels to write in the same
  tick.
- Runtime cost is the weak spot: 4 of 38 scanned release startup first ticks
  overran the `250 ms` period, with the worst at about `1364.773 ms`.
- Post-startup authority loss exists too: the current event log had 96
  post-startup `control_loop.authority_reasserted` events, so repeated reasserts
  after startup remain operational evidence to investigate.

## Soundness Addendum

This review is sound only when scoped to the current shipped release profile. It
is not sound as a universal claim about every historical or manual config.

Real release data supports the current-profile duty-step claim. I scanned 38
recent release startup CSV rows under `release\runtime\logs\archive`: every
startup row had all six controlled channels in non-Control mode (`mode_raw != 0`)
and every channel wrote `authority_reassert` at tick 1. Across those rows, the
largest first-tick absolute duty movement was `0.805%`, matching the configured
per-channel `max_setpoint_step_pct` envelope rather than a jump to full curve
demand. The latest startup (`2026-06-19T23:48:47`) was warm
(`cpu_tctl_c=66.625 C`): first-tick deltas were `-0.60`, `-0.80`, `+0.70`,
`+0.70`, `-0.60`, and `-0.80` percentage points for channels `0-5`.

Historical data refutes the same statement if it is not scoped. The older
`release\runtime\manual-90-smoke\runtime-run` capture at
`2026-06-06T20:56:15` (`cpu_tctl_c=81.500 C`) wrote startup setpoints of
`90-100%`, with first-tick deltas from about `36` to `75` percentage points. That
manual run does not invalidate the current release profile, but it proves the
small-step behavior depends on the current ramp/step-cap configuration.

Control-theory reasoning supports immediate reassertion as an authority recovery
action: if the observed actuator mode is not the controller-owned mode, the
controller's internal output state and the plant actuator state have diverged.
Reasserting the current bounded setpoint restores actuator authority and makes
the next control update meaningful. The control-theory argument does not require
that all channels be written in the same tick; global inter-channel staggering is
an implementation/scheduling choice, not a stability requirement.

The event/runtime cost evidence is mixed. The latest startup row had first-tick
work duration `40.496 ms`, well inside the `250 ms` period, and the next row
showed channels `0-5` back at `mode_raw=0`. Across the same 38 startup rows,
however, 4 first ticks exceeded the `250 ms` period; the worst first-tick work
duration was about `1364.773 ms`. That does not prove the write burst is unsafe,
but it means the "not too aggressive" conclusion is better supported for duty
magnitude than for startup tick cost.

The event log also shows authority loss can happen after startup. In the current
event log, `1675` `control_loop.authority_reasserted` events were present:
`1579` at tick 1 and `96` after startup. A later event at
`2026-06-19T23:40:26` shows channel `3` reasserting after observed
`mode_raw=111`. This is consistent with the design: startup reasserts are normal,
while repeated post-startup reasserts are operational evidence to investigate.

## Evidence

- `docs/CONTROL_LOOP.md` documents the control-loop startup/tick sequence:
  capture baselines, compute setpoints, skip writes blocked by gates, then write
  pending-sidecar entries before applied writes. It also documents continuous
  hold (`control_hold_ms=0`) as holding writes until shutdown or restart.
- `config/control.release.json` ships `poll_tick_ms=250`,
  `write_cooldown_ms=250`, `deadband_pct=0.25`, `control_hold_ms=0`, and six
  controlled channels: `0`, `4`, `3`, `2`, `5`, `1`. Every shipped live channel
  has an explicit `max_setpoint_step_pct` cap (`0.6`, `0.7`, or `0.8` percent).
- `docs/CONTROL_PIPELINE_MATH.md` defines authority reassert as a continuous-hold
  drift check. It bypasses the deadband, uses cooldown, and bypasses cooldown
  only for `first_write`.
- `src/control/channel_write.cpp` captures the observed fan duty/mode as the
  baseline and initializes `last_issued_pct` from observed duty. That means a
  startup reassert is not a blind first-write jump; duty movement still goes
  through the evaluator's setpoint rate limiter.
- `src/control/channel_evaluator.cpp` flags authority reassert when
  `effective_hold_ms == 0` and the observed fan mode is not Control-owned mode
  (`mode_raw != 0`) or duty has drifted beyond tolerance. It then applies the
  existing cooldown calculation.
- `src/hardware/sio_fan_writer.cpp` applies each accepted channel write with one
  direct `set_fan_duty` operation, with transient retry handling. There is no
  global startup write queue or inter-channel spacing layer.

## Aggressiveness Assessment

For the current release profile, the burst is acceptable as current behavior
because it reduces, rather than expands, authority ambiguity: if another writer
or firmware mode owns the fans at startup, Control immediately takes back the
configured live set `0-5`.

Under the current release profile, thermal/acoustic movement is bounded. Even if
CPU is warm, the first computed setpoint can move only by the channel's
configured `max_setpoint_step_pct` from the observed baseline, then subsequent
writes are per-channel cooldown-gated. This prevents a warm-start jump directly
to the full curve demand for that profile.

The missing bound is global bus/write spacing. If SIO bus contention, audible
simultaneous write noise, or startup-loop latency proves problematic, the next
change should not be an ad hoc throttle. It should be promoted as feature work
with acceptance criteria, because it would alter the shipped startup/write
cadence behavior.

## Follow-Up Candidate

Add a non-live regression test that starts the simulated control loop with
multiple channels reporting non-Control mode and warm CPU input, then asserts:

- all configured live channels can reassert authority on startup;
- the first reasserted duty for each channel respects `max_setpoint_step_pct`;
- later writes remain bounded by per-channel `write_cooldown_ms`;
- analyzer/reporting guidance continues to treat startup reasserts separately
  from post-startup authority loss.

No runtime interaction was performed for this review.
