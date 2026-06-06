# FEAT-0003: Selectable control-law profile with hot-swap

**Project:** svg-mb-control
**Status:** Draft   **Version:** 0.1   **Updated:** 2026-06-03
**Namespace:** `REQ-PROFILE-*`
**Companion to:** `AGENTS.md`, `docs/CONTROL_LOOP.md`,
`docs/CONTROL_PIPELINE_MATH.md`, `docs/WRITE_ORCHESTRATION.md`,
`docs/RUNTIME_HOME.md`, `docs/MEASUREMENT_GATE.md`,
`docs/features/FEAT-0001-hot-swap-write-policy.md`
**Purpose:** let an operator select among named control *profiles* — where a
profile may specify a different control **law** (the current feed-forward
curve+overlay law, or a PID / PI / PD / P feedback law), not only different
tuning numbers — and change the active profile on a running controller at a tick
boundary without restarting the process.

> **Scope & intent (2026-06-03).** This spec is design-capture, not scheduled
> work. Per the maintainer, the feature is not believed to be a net benefit and
> is not planned. PID is recorded as the worked **example of the range** of
> control laws the per-channel seam must be able to host — a feedback law as
> different from the current feed-forward curve as possible — so the abstraction
> is designed for that breadth rather than curve-specific. Any implementation
> would be a demonstration. The directional choices are recorded in
> `docs/profile-hot-swap-decision-2026-06-03.md`.

## 1. Summary

Today the per-channel control law is fixed in code: `EvaluateChannel`
(`src/control/channel_evaluator.cpp:440-474`) implements one law — a feed-forward
temperature→duty curve (`EvaluatePrimarySetpoint`,
`channel_evaluator.cpp:243-283`; `LookupCurve`,
`src/policy/control_policy.cpp:68-101`) plus a CPU-override curve, demand
smoothing, four boost overlays, a low-band residual, and a per-channel rate
limiter. A profile is only a set of tuning numbers fed to that one law; there is
no way to run a different law (for example a PID controller that regulates
temperature to a target), and no way to change the active profile without
restarting with a different `--config`.

This feature introduces a single per-channel control-law seam
(`IChannelController`), makes the current law one implementation
(`CurveOverlayController`) with output identical to today for unchanged configs,
adds a `PidController` implementation that covers the P, PI, PD, and PID forms by
gain selection, and lets the operator select the active profile — including the
control-law kind per channel — at startup and at a tick boundary on a running
controller (build-then-validate-then-swap, mirroring
`docs/features/FEAT-0001-hot-swap-write-policy.md`).

The operator-visible outcome is: pick a profile (a control law plus its
parameters per channel); it takes effect on the next tick; any active fan
override is safely restored to its captured baseline before a swap that changes
the channel set.

## 2. Problem & motivation  *(promotion gate 1)*

This is a named code/contract gap, not an observed runtime failure — the
capability does not exist yet.

1. **The control law is hard-coded.** `tick_runner.cpp:241-243` calls the free
   function `EvaluateChannel`; there is no interface, so the law cannot be
   replaced or selected. Adding a second law (PID) today would mean branching
   inside `EvaluateChannel` on a config flag, which mixes two unrelated laws in
   one translation unit.
2. **The current law is feed-forward, not feedback.** It computes
   `duty = curve(temp) + overlays` (`channel_evaluator.cpp:453-467`, composing
   `EvaluatePrimarySetpoint`/`ApplyCpuOverride`/`UpdateDemandAndBoosts`/`ComputeFinalSetpoint`);
   it does not
   regulate temperature to a target. A PID/PI/PD/P controller regulates a
   measured temperature to a target temperature by trimming duty, which is a
   different computation with different state (an integral accumulator and a
   derivative term). There is no place to put that state or that math.
3. **Config and per-channel state are welded together.** `ChannelState`
   (`src/control/control_runtime_context.h:17-84`) bundles the channel `config`
   with the law's dynamic state (`boosts[]`, `smoothed_demand_pct`,
   `last_issued_pct`, the `low_band_*` accumulators). The constructor copies each
   `loop.channels[i]` into `ChannelState.config`
   (`src/control/control_runtime_context.cpp:17-21`). Swapping the law (or the
   profile) therefore has no defined seam for which state carries over and which
   resets.
4. **Profile selection is restart-only.** The supervisor bakes one
   `--config <path>` into the worker command line
   (`src/control/control_supervisor.cpp:259-270`); the loader parses and
   validates once (`src/control/control_loop_config.cpp:507-568`). There is no
   `--profile` surface and no runtime path to change the active profile.

## 3. Goals & non-goals

**Goals**
- A single per-channel control-law seam (`IChannelController`) with one virtual
  evaluation call replacing the direct `EvaluateChannel` call at
  `tick_runner.cpp:241`.
- The current curve+overlay law becomes `CurveOverlayController`, producing
  output identical to today's `EvaluateChannel` for an unchanged config.
- A `PidController` that regulates a measured temperature to a per-channel target
  temperature, covering the P, PI, PD, and PID forms by gain selection (a zero
  gain disables that term; pure-P is `Ki = Kd = 0`).
- Per-channel selection of the control-law kind and its parameters from config.
- A tick-boundary path to change the active profile — including the law kind per
  channel — on a running controller, build-then-validate-then-swap.
- Defined transition semantics for control-law dynamic state on a swap.

**Non-goals**
- Auto-tuning of PID gains. Gains are operator-supplied config.
- A graphical UI. Operator surface is the runtime-home request file + CLI,
  consistent with `docs/MEASUREMENT_GATE.md`.
- Changing the write policy. Write authority is FEAT-0001's scope; this feature
  changes the control law that produces a setpoint, not whether the setpoint is
  written.
- Multi-threaded / mid-write profile changes. Changes apply only at tick
  boundaries.
- Persisting a live profile change back to `config/*.json` by default. A live
  change is in-memory until restart unless a save action is added later.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| No fan write / authority change outside an explicit operator action | `AGENTS.md` §Live Runtime Safety | A profile change applies only from an explicit runtime-home request consumed at the tick boundary, and emits a `control_loop.profile_*` event for every applied or rejected change. |
| Shipped live behavior is the measured baseline | `docs/MEASUREMENT_GATE.md` | The characterized live behavior is the curve+overlay law on the shipped channel set. Switching a *writing* channel to PID is a new, uncharacterized live behavior and crosses the gate; PID is first available in a non-writing shadow/dry-run path (§5) until characterized (REQ-PROFILE-07). |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | `CONTROL_PIPELINE_MATH.md` is scoped to the curve+overlay law (now `CurveOverlayController`) and its output stays byte-identical (REQ-PROFILE-01); the PID law gets its own identity reference (REQ-PROFILE-10). |
| Runtime sidecar / status / manifest schema stays backward-compatible | `docs/RUNTIME_HOME.md` | New `controller` config key, request file, status, and CSV fields are additive; an absent `controller` key means the curve+overlay law, so existing configs and archives stay valid. |
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | The seam, both controllers, and the swap are in-repo; no external process or sibling repo is added. |

## 5. Behavior specification

Proposed behavior (not yet implemented):

- **Control-law seam.** A per-channel `IChannelController` exposes one evaluation
  method that returns a `ChannelEvaluation` (the existing result type,
  `channel_evaluator.h:30-47`), a `Reset()` that clears the law's dynamic state,
  and a `Kind()` identifier (for example `curve_overlay`, `pid`). The tick body
  calls the channel's controller instead of the free `EvaluateChannel`.
- **Curve+overlay law preserved.** `CurveOverlayController` wraps the current law.
  For a config with no `controller` key (or `controller: "curve_overlay"`), its
  output is identical to today's `EvaluateChannel`. The boost-sum operand order
  that `docs/CONTROL_PIPELINE_MATH.md` §5 marks load-bearing
  (`channel_evaluator.cpp:353-364`) is preserved.
- **PID law.** `PidController` selects a primary temperature the same way the
  curve law does (`SelectPrimaryCurveInput`, `channel_evaluator.cpp:198-237` —
  shared input selection, including `temp_blend` and the source-aware guard),
  computes `error = observed_temp_c − target_c`, and outputs
  `clamp(bias + Kp·error + Ki·∫error·dt + Kd·d(error)/dt, min_duty_pct, 100)`.
  The integral term is anti-windup-clamped. The P, PI, PD, and PID forms are the
  same controller with the unused gains set to zero. The exact derivative
  variant, the integral clamp, and the `bias`/feed-forward question are settled
  in the decision record (§9).
- **Shared output conditioning.** Clamp to `[min_duty_pct, 100]`, sensor-safe
  mode (100% on sustained sensor failure, `channel_evaluator.cpp:248-264`),
  deadband, write cooldown, control-hold, circuit breaker, baseline
  capture/restore, and the write gate (`channel_write.*`) are law-agnostic and
  apply identically regardless of controller kind. Which of the current
  curve-law output dynamics (the EMA demand smoothing + decay latch,
  `channel_evaluator.cpp:72-117`; the rise/fall/max-step rate limiter,
  `channel_evaluator.cpp:37-70`) are shared safety conditioning versus
  curve-law-specific is settled in §9.
- **Profile selection at startup.** Each channel's `controller` key selects the
  law; the loader (`LoadChannelConfig`, `control_loop_config.cpp:407-472`)
  dispatches on it and validates per law.
- **Hot-swap at a tick boundary.** A profile change is delivered as a
  runtime-home request file and consumed once per tick, mirroring the
  breaker-reset pattern (`TakeRuntimeBreakerResetRequest`,
  `src/runtime/runtime_lifecycle.cpp:55-103`; intake at `tick_runner.cpp:195`).
  Apply order: (1) parse + validate the candidate profile via
  `LoadControlLoopConfig` plus per-law validation — on failure keep the running
  profile, emit `control_loop.profile_rejected`, and stop; (2) for a channel-set
  change, restore baseline and clear `write_active` for dropped channels via the
  FEAT-0001 path (`RestoreSavedState` / `HandleExpiredHoldRestore`,
  `channel_write.h:38-48`); (3) build each channel's new controller, reset its
  dynamic state by default (§9), and swap `context.loop`, each
  `context.channels[i].config`, and each controller at the tick boundary; emit
  `control_loop.profile_applied` with the per-channel law/parameter delta.
- **Measurement-gate path for PID.** Until the PID law on a given channel is
  characterized, the PID controller runs in a non-writing shadow/dry-run path: it
  computes a setpoint and logs it (so its behavior can be compared to the curve
  baseline), but the write is suppressed for that channel — composing with
  FEAT-0001's write-policy gate. Authorizing live PID writes requires the
  characterization evidence named in `docs/MEASUREMENT_GATE.md`
  (REQ-PROFILE-07).
- **Atomicity.** The loop is single-threaded per tick; a change applies between
  ticks and never interleaves with an in-flight write.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-PROFILE-01 | A per-channel control-law interface (`IChannelController`) must replace the direct `EvaluateChannel` call at the tick body's single call site. The curve+overlay law must become an implementation whose output is identical to today's `EvaluateChannel` for an unchanged config. |
| REQ-PROFILE-02 | A `PidController` must implement the same interface and regulate a measured temperature to a per-channel target, covering the P, PI, PD, and PID forms by gain selection (a zero gain disables that term). It must not be a separate class per form. |
| REQ-PROFILE-03 | The control-law kind and its parameters must be selected per channel from config via a `controller` discriminator. An absent `controller` key must mean the curve+overlay law, so existing configs remain valid without edits. |
| REQ-PROFILE-04 | A profile change — including a change of control-law kind per channel — must be consumed at a tick boundary via a runtime-home request file, build-then-validate-then-swap. A validation failure must leave the running profile in force, leave no partial state, and emit a rejection event. |
| REQ-PROFILE-05 | On a swap, each channel's control-law dynamic state must be reset by default; any carry-over of dynamic state is opt-in and out of the first slice. |
| REQ-PROFILE-06 | Shared output conditioning (clamp to `[min_duty_pct, 100]`, sensor-safe mode, deadband, write cooldown, control-hold, circuit breaker, baseline capture/restore, write gate) must apply identically regardless of controller kind. |
| REQ-PROFILE-07 | Switching a writing channel to a control-law kind not in the characterized baseline crosses `docs/MEASUREMENT_GATE.md`. PID must be available first in a non-writing shadow/dry-run path that computes and logs but does not write; live PID writes require the characterization evidence named in the gate. |
| REQ-PROFILE-08 | The active control-law kind per channel must be recorded in the runtime status and CSV (additive fields), so an operator and the analyzer can attribute observed behavior to the law that produced it. |
| REQ-PROFILE-09 | A swap that changes the channel set (add/remove/reorder) must match channels by `channel` id and reuse the FEAT-0001 restore/capture choreography. If FEAT-0001 is not yet implemented, channel-set changes are out of scope and the swap must reject a candidate whose channel set differs. |
| REQ-PROFILE-10 | Introducing and maintaining the controllers must keep the control-identity docs current: `docs/CONTROL_PIPELINE_MATH.md` scoped to the curve+overlay law, and a sibling identity reference for the PID law (per `AGENTS.md` §Change Checklist). |

## 7. Data / schema deltas

- **New per-channel config key `controller`** (string; `curve_overlay` default).
  When `pid`, a `pid` object carries `target_c`, `kp`, `ki`, `kd`, an optional
  `form` label (`p`/`pi`/`pd`/`pid`, informational; the gains are authoritative),
  the derivative variant flag, the integral clamp bounds, and `bias`/feed-forward
  selection (exact field set settled in §9). `min_duty_pct` and the output slew
  cap are shared output fields, reused from the existing channel schema.
- **New runtime-home request file** (for example
  `runtime/requests/profile.json`): names the target profile or carries an inline
  profile body. Absence = no change. Modeled on the breaker-reset request file.
- **New status + CSV fields:** active `controller` kind per channel; for PID, the
  current error and per-term contributions for evidence. Additive; absent in old
  archives.
- **New event types** `control_loop.profile_applied` /
  `control_loop.profile_rejected` / `control_loop.profile_invalid`.
- **Schema/version impact:** additive only; update `docs/RUNTIME_HOME.md`
  (request file, status fields, events) and `docs/CONTROL_LOOP.md` at
  implementation. No existing config or runtime-home file becomes invalid.

## 8. CLI / config / operator surface deltas

- **New operator action** to write the profile request file (a subcommand
  parallel to the existing breaker-reset operator path), respecting `AGENTS.md`
  §Live Runtime Safety: an explicit, opt-in action.
- **Optional `--profile <name>` startup selector** if profiles are stored as
  named files; otherwise `--config` continues to carry the startup profile.
- Update `README.md` (operator workflow), `docs/CONTROL_LOOP.md`,
  `docs/WRITE_ORCHESTRATION.md`, and `docs/RUNTIME_HOME.md` per `AGENTS.md`
  §Change Checklist.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/profile-hot-swap-decision-2026-06-03.md`](../profile-hot-swap-decision-2026-06-03.md) | The control-law seam shape; where control-law dynamic state lives (in `ChannelState` vs. owned by the controller); the PID structure (derivative variant, integral anti-windup, and whether PID trims a feed-forward bias/curve or runs from a fixed bias); which current output dynamics are shared safety conditioning vs. curve-law-specific; the state carry-over-vs-reset default on swap; the measurement-gate path (shadow/dry-run before live PID); and whether channel-set changes are in the first slice. | Proposed — awaits maintainer acceptance |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-PROFILE-01 | T, R | `.\scripts\Test-LocalCI.ps1` — a `CurveOverlayController` output-equivalence test vs. the current `EvaluateChannel` over representative inputs; review confirms the single call site |
| REQ-PROFILE-02 | T | unit test: one `PidController` produces P, PI, PD, PID behavior under gain selection; pure-P with `Ki=Kd=0` holds no integral/derivative term |
| REQ-PROFILE-03 | T, R | config-load tests: absent `controller` → curve law; `controller: "pid"` → PID; per-law validation rejects malformed fields |
| REQ-PROFILE-04 | T | test: profile request applied at tick boundary; a validation failure on swap retains the running profile and emits a rejection event |
| REQ-PROFILE-05 | T | test: after a swap, the new controller's dynamic state is reset (integral = 0, no carried smoothing/boost) |
| REQ-PROFILE-06 | T, R | test: sensor-safe mode, deadband, cooldown, breaker, and clamp behave identically for both controller kinds; review vs. `channel_write.*` |
| REQ-PROFILE-07 | R, M | review vs. `docs/MEASUREMENT_GATE.md`; runtime evidence that PID runs shadow/dry-run before any live PID write |
| REQ-PROFILE-08 | T | CSV/status header+row tests assert the per-channel controller-kind field is present and correct |
| REQ-PROFILE-09 | T, R | test: a candidate with a differing channel set is rejected when FEAT-0001 is absent; when present, dropped channels restore and added channels capture baseline |
| REQ-PROFILE-10 | R | review: `CONTROL_PIPELINE_MATH.md` scoped to curve+overlay and unchanged in value; PID identity reference exists |

Legend: T = `Test-LocalCI` automated test; B = build/release gate; M = manual
runtime measurement (respecting Live Runtime Safety); R = code review vs. the
cited contract.

## 11. Open decisions

| Decision | Needed before | Current lean |
|---|---|---|
| Where control-law dynamic state lives | implementation (§9) | **Decided 2026-06-03:** the controller owns its dynamic state; full decouple in one pass, `ChannelState` slimmed to law-agnostic fields (§9 D2) |
| PID structure — fixed-bias vs. curve-trim | implementation (§9) | **Decided 2026-06-03:** selectable via `pid.feedforward` (`curve`\|`fixed`) (§9 D3a) |
| Live authorization — shadow/dry-run vs. live behind a flag | implementation (§9) | **Decided 2026-06-03:** shadow/dry-run default; live via explicit `pid.allow_live` opt-in (§9 D6) |
| Are the EMA smoothing + rate limiter shared safety conditioning or curve-law-specific? | implementation (§9) | Rate/step cap shared as a safety slew clamp; EMA smoothing + decay latch curve-law-specific |
| Channel-set changes in the first slice? | implementation | No — first slice is same channel set, law/params only (depends on FEAT-0001 for the general case) |
| Does a live profile change persist to config, or stay in-memory until restart? | implementation | In-memory only (startup config/profile is the durable source) |

## 12. Measurement gate & dependencies

- **Measurement gate:** changing tuning numbers within the curve+overlay law does
  not move the baseline. Switching a *writing* channel to the PID law is a new
  live control behavior and **does** cross `docs/MEASUREMENT_GATE.md`; PID must
  run in the non-writing shadow/dry-run path until characterized
  (REQ-PROFILE-07).
- **Depends on:** the request-intake + build-then-swap mechanism is shared with
  `docs/features/FEAT-0001-hot-swap-write-policy.md`; channel-set changes on a
  swap depend on FEAT-0001's restore/capture path (REQ-PROFILE-09). Evidence for
  comparing PID against the curve baseline composes with
  `docs/features/FEAT-0002-cpu-settings-evidence-logger.md` and the runtime
  logger.
- **Build/test impact:** new controller modules and tests under `tests/`; doc
  updates per `AGENTS.md` §Change Checklist; a PID control-identity doc plus a
  curve-law-scoped `CONTROL_PIPELINE_MATH.md`.

## 13. Promotion-gate checklist

- [x] 1. Problem stated as a named code/contract gap with file:line evidence (§2).
- [x] 2. Stressed invariants identified — Live Runtime Safety, Measurement Gate, control identity, RUNTIME_HOME schema, Repo Boundary (§4).
- [x] 3. Design decision record written; directional choices D2/D3a/D6 recorded 2026-06-03 (§9). Implementation not scheduled — see Scope & intent.
- [x] 4. Concrete `REQ-PROFILE-*` IDs assigned (§6).
- [x] 5. Verification mapped to `Test-LocalCI` / review / runtime evidence (§10).
- [x] 6. Confirmed it does not violate Live Runtime Safety or Repo Boundary; the live-PID path is explicitly gated by the Measurement Gate rather than moving the baseline silently.
- [x] 7. Doctrine check: claims grounded with file:line; proposed behavior labeled as proposed; `must`/`should`/`is` used per `CLAUDE.md`; PID expanded as proportional-integral-derivative on first use below.

> The decision record (§9) exists and records the directional choices, so the
> promotion gates are met on paper. The spec stays `Draft` because implementation
> is not scheduled (see Scope & intent): it is design-capture of the required
> *range* of swappable control laws, not authorized work. PID =
> proportional-integral-derivative; P / PI / PD are its degenerate forms with one
> or more gains set to zero.

## 14. Verification log  *(fill in after the feature is built)*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-PROFILE-01 | | | |
| REQ-PROFILE-02 | | | |
| REQ-PROFILE-03 | | | |
| REQ-PROFILE-04 | | | |
| REQ-PROFILE-05 | | | |
| REQ-PROFILE-06 | | | |
| REQ-PROFILE-07 | | | |
| REQ-PROFILE-08 | | | |
| REQ-PROFILE-09 | | | |
| REQ-PROFILE-10 | | | |

**Spec vs. implementation deltas:** <record anything built differently from this
spec, and why. Update §5/§6 and the cited contract docs if behavior changes, and
bump **Updated**.>
