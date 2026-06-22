# FEAT-0003: Restart-selected control-law profile seam

**Project:** svg-mb-control
**Status:** Done   **Version:** 0.5   **Updated:** 2026-06-22
**Namespace:** `REQ-PROFILE-*`
**Companion to:** `AGENTS.md`, `docs/CONTROL_LOOP.md`,
`docs/CONTROL_PIPELINE_MATH.md`, `docs/WRITE_ORCHESTRATION.md`,
`docs/RUNTIME_HOME.md`, `docs/MEASUREMENT_GATE.md`,
`docs/features/FEAT-0023-machine-profiles-and-restart-switch.md`,
`docs/profile-hot-swap-decision-2026-06-03.md`,
`docs/multiprofile-restart-switch-decision-2026-06-20.md`
**Purpose:** define the per-channel PID/control-law seam that a resolved machine
profile can select at worker startup. The active law is fixed for the worker
lifetime and changes only through the FEAT-0023 restart-based profile switch, not
through an in-process tick-boundary swap.

> **Done / implementation verified (2026-06-22).** FEAT-0023 (the profile
> catalog + restart switch) is Implemented and validated, clearing the sole
> sequencing gate that kept this spec Draft; all seven promotion gates were
> already met. This seam lets a resolved profile choose `curve_overlay` or a
> PID-family law per channel at worker startup. The seam +
> `CurveOverlayController` (output-identical) + `PidController` (shadow/dry-run by
> default) + config dispatch + the measurement-gate preconditions are built. The
> REQ-PROFILE-07 live M gate is closed for the channel-0-only opt-in proven in
> [`docs/pid-live-channel0-evidence-2026-06-22.md`](../pid-live-channel0-evidence-2026-06-22.md):
> `pid.allow_live` + characterization evidence + a positive non-NaN slew cap
> enabled one short live run, then the package rolled back to the shipped
> `curve_overlay` default. The 2026-06-21 shadow characterization still rejects
> all-channel live PID.
>
> The 2026-06-03 decision record remains the source for the seam shape, PID
> structure, controller-owned dynamic state, shared output conditioning, and the
> PID measurement-gate posture. The 2026-06-20 FEAT-0023 decision supersedes the
> older in-process hot-swap pieces: no runtime request is consumed by the tick
> runner, no law changes mid-worker, and channel-set switching is FEAT-0023's
> profile/catalog concern.

## 1. Summary

Today the per-channel control law is fixed in code: `EvaluateChannel`
(`src/control/channel_evaluator.cpp:440-474`) implements one feed-forward
temperature-to-duty curve (`EvaluatePrimarySetpoint`,
`channel_evaluator.cpp:243-283`; `LookupCurve`,
`src/policy/control_policy.cpp:68-101`) plus a CPU-override curve, demand
smoothing, boost overlays, low-band residual logic, and a per-channel rate
limiter. A profile can tune that one law, but cannot choose a different law such
as a proportional-integral-derivative (PID) feedback controller.

This feature introduces a per-channel `IChannelController` seam. The existing
curve/overlay law becomes `CurveOverlayController` and must produce the same
output as today's `EvaluateChannel` for unchanged configs. A `PidController`
covers P, PI, PD, and PID by gain selection. Each channel's resolved config can
select the controller kind; an absent selector means `curve_overlay`, preserving
all existing configs.

Selection is restart-based. FEAT-0023 owns named profiles, machine identity, and
the operator profile switch. When FEAT-0023 restarts the worker into a different
resolved profile, FEAT-0003 lets the new worker construct the requested law per
channel. The running worker never swaps laws in process.

## 2. Problem & motivation  *(promotion gate 1)*

This is a named code/contract gap, not an observed runtime failure.

1. **The control law is hard-coded.** `tick_runner.cpp:241-243` calls the free
   function `EvaluateChannel`; there is no interface, so a second law would have
   to branch inside the same evaluator.
2. **The current law is feed-forward, not feedback.** It computes
   `duty = curve(temp) + overlays` (`channel_evaluator.cpp:453-467`) and does
   not regulate temperature to a target. PID needs an error term, an integral
   accumulator, and derivative memory.
3. **Config and dynamic state are welded.** `ChannelState`
   (`src/control/control_runtime_context.h:17-84`) stores the channel config
   beside law-specific state (`boosts[]`, `smoothed_demand_pct`,
   `last_issued_pct`, `low_band_*`). The constructor copies each
   `loop.channels[i]` into `ChannelState.config`
   (`src/control/control_runtime_context.cpp:17-21`), leaving no clean seam for
   controller-owned state.
4. **Profiles can be selected only by config path today.** The supervisor bakes
   one config into the worker command line and each worker re-reads it at
   startup. FEAT-0023 adds named profile resolution and restart switching; this
   spec defines what a profile can select inside a channel once that machinery
   exists.

## 3. Goals & non-goals

**Goals**

- A single per-channel control-law interface (`IChannelController`) replacing the
  direct `EvaluateChannel` call at the tick body's single call site.
- A `CurveOverlayController` implementation whose output is identical to today's
  curve/overlay evaluator for unchanged configs.
- A `PidController` that regulates a measured temperature to a per-channel target
  and covers P, PI, PD, and PID forms by gain selection.
- Per-channel controller-kind selection from the resolved startup profile, with
  `curve_overlay` as the default when the selector is absent.
- Controller-owned dynamic state, initialized on worker startup/restart and not
  carried across profile changes.
- Shared output conditioning and write authority that are law-agnostic.
- Additive status/CSV reporting so runtime evidence can attribute behavior to
  the controller kind that produced it.

**Non-goals**

- No in-process, mid-tick, or tick-boundary law swap. FEAT-0023 changes profiles
  by restarting the worker.
- No profile catalog, machine identity, operator switch request, or supervisor
  restart path. Those are FEAT-0023.
- No channel add/remove/reorder support in this seam. FEAT-0023 validates and
  switches whole profiles; this feature assumes the resolved channel set is valid
  for the machine profile.
- No auto-tuning of PID gains. Gains are operator-supplied config.
- No GUI and no sibling-repo, subprocess bridge, or external sensor dependency.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| No fan write / authority change outside an explicit operator action | `AGENTS.md` Live Runtime Safety | This spec adds no live operator action. Law changes happen only when FEAT-0023 explicitly restarts the worker into another profile. |
| Shipped live behavior is the measured baseline | `docs/MEASUREMENT_GATE.md` | The default `curve_overlay` law reproduces today's behavior. A live PID write crosses the gate and is rejected unless `pid.allow_live` has characterization evidence plus a positive non-NaN slew cap. |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | The current identity remains scoped to `CurveOverlayController`; PID gets its own identity reference before implementation handoff. |
| Runtime sidecar / status / CSV schema stays backward-compatible | `docs/RUNTIME_HOME.md` | The `controller` config key and controller-kind status/CSV fields are additive; absent means `curve_overlay`; old archives remain valid. |
| Repo stays standalone | `AGENTS.md` Repo Boundary | The seam and controllers live in this repo and use existing sensor/write abstractions. |

## 5. Behavior specification

Implemented behavior.

- **Control-law seam.** A per-channel `IChannelController` exposes one
  evaluation method returning the existing `ChannelEvaluation` result, plus a
  `Kind()` identifier and controller-owned reset/initialization. The tick body
  calls the channel's controller instead of the free `EvaluateChannel`.
- **Curve/overlay law preserved.** `CurveOverlayController` wraps the current
  evaluator. With no `controller` key, or with `controller: "curve_overlay"`, it
  must produce output identical to today's `EvaluateChannel`; the boost-sum
  operand order documented as load-bearing in `docs/CONTROL_PIPELINE_MATH.md`
  stays unchanged.
- **PID-family law.** `PidController` selects its primary temperature through the
  same source-selection path as the curve law, computes an error against
  `target_c`, and outputs a clamped duty from a fixed or curve feed-forward bias
  plus `Kp`, `Ki`, and `Kd` terms. P, PI, PD, and PID are one implementation with
  unused gains set to zero.
- **Resolved-profile selection at startup.** `LoadChannelConfig` dispatches on a
  per-channel `controller` discriminator after FEAT-0023 resolves the active
  profile. Invalid law config fails the worker's startup/config validation and
  therefore keeps FEAT-0023's validate/revert behavior in charge of switching.
- **Worker-lifetime dynamic state.** Controller dynamic state is created with the
  worker, scoped to that worker lifetime, and discarded on worker exit. A profile
  switch that changes laws restarts the worker; no curve smoothing, boost state,
  or PID integral/derivative state carries across that restart.
- **Shared output conditioning.** Clamp to `[min_duty_pct, 100]`, sensor-safe
  mode, deadband, write cooldown, control-hold, circuit breaker, baseline
  capture/restore, and the write gate are law-agnostic and apply identically
  regardless of controller kind. The rate/step cap is shared as the safety slew
  bound; curve EMA smoothing and decay-latch behavior stay curve-law-specific.
- **Measurement-gate path for PID.** PID is shadow/dry-run by default: it
  computes and logs a setpoint but does not write for that channel. A live PID
  write requires explicit per-channel `pid.allow_live: true`; an `allow_live`
  that lacks either the characterization evidence (a shadow-log comparison
  against the curve baseline is accepted) or a positive non-NaN slew cap is
  **downgraded to shadow/dry-run at controller construction** — the channel runs
  shadow rather than failing the worker, so one mis-evidenced channel cannot stop
  the rest. Malformed PID config (missing `target_c`, invalid gains, fixed
  feed-forward without `fixed_feedforward_pct`, `integral_min >= integral_max`)
  still fails config load. A live PID channel uses the shared safety floor, and
  the slew cap is enforced per tick by the shared `RateLimitSetpoint`.
- **Observability.** Runtime status and the standard control-loop CSV record the
  controller kind per channel. Law-specific fields are kind-aware or nullable so
  curve-only values are not published as meaningful PID values. An unknown or
  misspelled `controller` key is ignored and the channel runs the default curve
  law (fail-safe: a typo never yields an unguarded live PID), consistent with the
  loader's permissive unknown-key handling elsewhere.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-PROFILE-01 | A per-channel control-law interface (`IChannelController`) must replace the direct `EvaluateChannel` call at the tick body's single call site. The curve/overlay law must become an implementation whose output is identical to today's `EvaluateChannel` for an unchanged config. |
| REQ-PROFILE-02 | A `PidController` must implement the same interface and regulate a measured temperature to a per-channel target, covering the P, PI, PD, and PID forms by gain selection; it must not be a separate class per form. |
| REQ-PROFILE-03 | The control-law kind and parameters must be selected per channel from the resolved startup profile via a `controller` discriminator. An absent `controller` key must mean the curve/overlay law, so existing configs remain valid without edits. |
| REQ-PROFILE-04 | A control-law change must be restart-selected through FEAT-0023 profile resolution: the worker loads the selected law at startup, and a different law requires the FEAT-0023 supervisor restart switch. This feature must not add an in-process tick-boundary law-swap request or partial live state swap. |
| REQ-PROFILE-05 | Controller dynamic state must be owned by the controller and scoped to one worker lifetime. Worker startup/restart must initialize fresh controller state; no curve smoothing/boost state or PID integral/derivative state may carry across a FEAT-0023 profile switch. |
| REQ-PROFILE-06 | Shared output conditioning (clamp to `[min_duty_pct, 100]`, sensor-safe mode, deadband, write cooldown, control-hold, circuit breaker, baseline capture/restore, write gate, and safety slew cap) must apply identically regardless of controller kind. |
| REQ-PROFILE-07 | Switching a writing channel to a control-law kind not in the characterized baseline crosses `docs/MEASUREMENT_GATE.md`. PID must be available first in a non-writing shadow/dry-run path that computes and logs but does not write. A live PID write must require the explicit per-channel `pid.allow_live` opt-in; an `allow_live` that lacks either the characterization evidence or a positive non-NaN slew cap must be downgraded to shadow/dry-run at controller construction (the channel runs shadow, it does not fail the worker), and the slew cap must be enforced per tick. Malformed PID config fails config load. |
| REQ-PROFILE-08 | The active control-law kind per channel must be recorded in runtime status and CSV as additive fields. Law-specific reporting fields must be kind-aware or nullable; curve-only values such as `feedforward_pct` / `last_raw_demand_pct` must not be published as if they were meaningful for PID channels. |
| REQ-PROFILE-09 | FEAT-0003 must not add channel-set switching semantics. Channel add/remove/reorder validation remains owned by the resolved machine profile and FEAT-0023; this seam assumes a valid channel set and only selects the law and parameters for each channel loaded in that worker. |
| REQ-PROFILE-10 | Introducing and maintaining the controllers must keep the control-identity docs current: `docs/CONTROL_PIPELINE_MATH.md` scoped to the curve/overlay law, and a sibling identity reference for the PID law before implementation handoff. |

## 7. Data / schema deltas

- **New per-channel config key:** `controller` (string; `curve_overlay` default).
  When `pid`, a `pid` object carries `target_c`, `kp`, `ki`, `kd`, optional
  informational `form`, derivative/integral settings, feed-forward selection, and
  `pid.allow_live` (default false). The exact field names are settled during
  implementation without changing the requirements above.
- **No new FEAT-0003 runtime-home request file.** FEAT-0023 owns
  `profile.switch.request.json`; this feature only affects what the restarted
  worker loads from the resolved profile.
- **New status + CSV fields:** active controller kind per channel; PID evidence
  fields such as error and per-term contribution are nullable/kind-aware.
- **Schema/version impact:** additive only. Update `docs/RUNTIME_HOME.md`,
  `docs/CONTROL_LOOP.md`, `docs/RUNTIME_LOGGING_AND_EVALUATION.md`, and analyzer
  schema/tests if the controller fields are ingested.

## 8. CLI / config / operator surface deltas

- No FEAT-0003-specific operator command.
- FEAT-0023 owns `--profile`, `SVG_MB_PROFILE`, identity resolution, and the
  restart-switch request.
- `--show-config` should include the resolved per-channel controller kind after
  FEAT-0023 profile resolution.
- Update `README.md`, `docs/CONTROL_LOOP.md`, `docs/WRITE_ORCHESTRATION.md`,
  `docs/RUNTIME_HOME.md`, and the PID identity doc during implementation.

## 9. Design decision record(s)  *(promotion gate 3 - write before implementation)*

| Decision doc | Decision it settles | Status |
|---|---|---|
| [`docs/profile-hot-swap-decision-2026-06-03.md`](../profile-hot-swap-decision-2026-06-03.md) | Control-law seam shape; controller-owned dynamic state; PID structure; shared output conditioning; PID measurement-gate posture (`pid.allow_live` requires characterization evidence and a non-NaN positive slew cap). Its in-process swap pieces are historical for the current restart-selected scope. | Current for seam/PID direction; in-process swap parts superseded. |
| [`docs/multiprofile-restart-switch-decision-2026-06-20.md`](../multiprofile-restart-switch-decision-2026-06-20.md) | FEAT-0003 is restart-selected under FEAT-0023, law fixed per worker lifetime, sequenced after the profile catalog and restart switch. | Current |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-PROFILE-01 | T, R | `.\scripts\Test-LocalCI.ps1` - a `CurveOverlayController` output-equivalence test vs. current `EvaluateChannel`; review confirms the single call site. |
| REQ-PROFILE-02 | T | Unit tests: one `PidController` produces P, PI, PD, and PID behavior under gain selection; pure-P with `Ki = Kd = 0` holds no integral/derivative term. |
| REQ-PROFILE-03 | T, R | Config-load tests: absent `controller` selects curve law; `controller: "pid"` selects PID; per-law validation rejects malformed fields after FEAT-0023 profile resolution. |
| REQ-PROFILE-04 | T, R | FEAT-0023 integration test: a profile switch restarts the worker into the new law; no FEAT-0003 runtime request or tick-boundary swap path exists. |
| REQ-PROFILE-05 | T | Startup/restart tests: new controller instances start with clean dynamic state; no PID integral/derivative or curve smoothing/boost state is reused across a profile switch. |
| REQ-PROFILE-06 | T, R | Tests: sensor-safe mode, deadband, cooldown, breaker, clamp, write gate, and slew cap behave identically for controller kinds; review vs. `channel_write.*`. |
| REQ-PROFILE-07 | T, R, M | Config-load test rejects `pid.allow_live: true` without characterization evidence and a positive non-NaN slew cap; review vs. `docs/MEASUREMENT_GATE.md` and decision D6; runtime evidence that PID is shadow/dry-run by default and live only under an evidenced opt-in (`docs/pid-live-channel0-evidence-2026-06-22.md`). |
| REQ-PROFILE-08 | T, R | CSV/status header+row tests assert per-channel controller-kind fields and kind-aware/nullable law-specific reporting; analyzer tests if ingested. |
| REQ-PROFILE-09 | R | Review confirms FEAT-0003 adds no channel-set switch path and delegates profile/channel-set validation to FEAT-0023. |
| REQ-PROFILE-10 | R | Review: `CONTROL_PIPELINE_MATH.md` remains the curve/overlay identity reference and a PID identity reference exists. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` Live Runtime Safety).
- **R** = code review against the cited contract doc, decision record, or source.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Exact PID config field layout and validation diagnostics | implementation | Keep D3a's `feedforward: curve|fixed`; use gains as authoritative; keep `form` informational if present. |
| Derivative variant and anti-windup details | implementation | Derivative-on-measurement; clamp plus conditional integration; back-calculation deferred. |
| Exact PID identity-doc filename and formulas | implementation handoff | Add a sibling PID identity reference and leave `CONTROL_PIPELINE_MATH.md` as the curve/overlay identity. |
| Whether PID evidence fields are ingested by analyzer or status/CSV only | implementation | Emit additive CSV/status fields; bump analyzer schema only if ingested. |

## 12. Measurement gate & dependencies

- **Measurement gate:** the default `curve_overlay` law must reproduce the
  shipped behavior and does not move the baseline. A writing PID channel is new
  live control behavior and crosses `docs/MEASUREMENT_GATE.md`; PID starts in
  shadow/dry-run and `pid.allow_live` is downgraded to shadow at controller
  construction unless characterization evidence and a positive non-NaN slew cap
  are present.
- **Depends on:** FEAT-0023 for profile catalog, startup profile resolution, and
  restart-based profile switching. FEAT-0003 should be implemented only after
  FEAT-0023's default-profile and switch behavior are validated.
- **Build/test impact:** new controller modules and tests under `tests/`; docs
  updates per `AGENTS.md` Change Checklist; additive runtime/reporting fields and
  optional analyzer schema update if ingested.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem stated as a named code/contract gap with file:line evidence (section 2).
- [x] 2. Stressed invariants identified - Live Runtime Safety, Measurement Gate, control identity, runtime schema stability, Repo Boundary (section 4).
- [x] 3. Required design decision records written and marked Current for the active direction (section 9).
- [x] 4. Concrete `REQ-PROFILE-*` IDs assigned from the reserved namespace (section 6).
- [x] 5. Verification mapped to real checks and mirrored in `docs/TRACEABILITY.md` (section 10).
- [x] 6. Confirmed it does not violate Live Runtime Safety or Repo Boundary and does not silently move the Measurement Gate baseline (sections 4 and 12).
- [x] 7. Doctrine check: claims grounded with file:line; proposed behavior labeled proposed; no undefined or unqualified vague terms.

> All seven promotion gates are met. **Promoted Draft -> Accepted 2026-06-21**:
> FEAT-0023 is Implemented + validated, clearing the sequencing gate, and the
> maintainer authorized execution. Implementation and verification are complete
> as of 2026-06-22. The live PID write gate (REQ-PROFILE-07 M) is proven for the
> evidenced channel-0-only opt-in in
> [`docs/pid-live-channel0-evidence-2026-06-22.md`](../pid-live-channel0-evidence-2026-06-22.md);
> the shipped default remains `curve_overlay`, and all-channel live PID remains
> rejected by `docs/pid-shadow-characterization-2026-06-21.md`.

## 14. Verification log  *(fill in after the feature is built - "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-PROFILE-01 | pass | `IChannelController` seam; single call site `tick_runner.cpp` `controllers[i]->Evaluate`; `CurveOverlayController` forwards to `EvaluateChannel`. Forward-equivalence test (`channel_controller_tests`) + 31/31 `test_control_loop.py` through the new dispatch unchanged. Commit `2fec980`. | 2026-06-21 |
| REQ-PROFILE-02 | pass | One `PidController` covers P/PI/PD/PID by gain selection. `pid_controller_tests`: pure-P, integral accumulation, derivative sign, derivative-on-measurement no-kick, anti-windup freeze, integral clamp. Commit `9f4b79d`. | 2026-06-21 |
| REQ-PROFILE-03 | pass | `controller` discriminator + nested `pid` parsed in `LoadChannelConfig`; absent key defaults to `curve_overlay`; malformed pid + unknown controller/feedforward throw at load. `control_loop_config_tests` (10 cases). Commit `31a5e6d`. | 2026-06-21 |
| REQ-PROFILE-04 | pass (R) | Restart-selected: the law is built once in `CreateChannelController` at worker construction; the tick runner consumes no FEAT-0003 request and has no tick-boundary law-swap path. FEAT-0023 owns the supervised restart switch (`test_profile_switch.py`). | 2026-06-21 |
| REQ-PROFILE-05 | partial | Functional pass: `PidController` owns its `PidState`; all controller state is created with the worker and discarded on exit, and a FEAT-0023 switch restarts into fresh instances (`Reset()` + ctor). Architectural decouple of the curve law's state out of `ChannelState` is a **deliberate deferred partial** under the restart-selected scope (decision D2 2026-06-21 note); no behavioral consequence. | 2026-06-21 |
| REQ-PROFILE-06 | pass | Shared output conditioning is law-agnostic: PID reuses the hoisted `SelectPrimaryCurveInput` + `RateLimitSetpoint`, applies the same `[min_duty,100]` clamp, shares the `channel_write.cpp` write path (deadband/cooldown/baseline/breaker), and reuses sensor-safe mode (`pid_controller_tests` safe-mode case). Commit `9f4b79d`. | 2026-06-21 |
| REQ-PROFILE-07 | pass | Shadow/dry-run default + decision-D6 gate. `pid_controller_tests` `PidLiveAuthorized` cases (allow_live/slew/artifact); `test_control_loop.py` pid-shadow e2e: `total_writes=0` after 25+ ticks at 75 °C, `pid_shadow` startup event. Write-path `write_suppressed` early-return before any actuation. First real-archive shadow replay: `docs/pid-shadow-characterization-2026-06-21.md` rejects all-channel live PID and identifies only channel 0 as a plausible gated live experiment. Channel-0 live M passed 2026-06-22 using `Kp=0.3`, `Ki=0.01`, `Kd=0.0`, target `68 C`, `allow_live=true`, the characterization artifact in the release package, and `max_setpoint_step_pct=0.6`; the 60 s hold stayed healthy with channel 0 `controller_kind=pid` and rolled back to the packaged `curve_overlay` default. Evidence: `docs/pid-live-channel0-evidence-2026-06-22.md`. Commits `9f4b79d`, `490f7f0`, package `3ce1c346bcf5`. | 2026-06-22 |
| REQ-PROFILE-08 | pass | Additive `controller_kind` + `pid_error_c/p/i/d_term/setpoint_raw_pct` in CSV (table-driven, append-only, bind-by-name) and JSON status (kind-aware: `null` for curve). `controller_kind` set from `IChannelController::Kind()` (single source of truth). pid-shadow e2e asserts `controller_kind=pid`, blanked `feedforward_pct`. Commit `490f7f0`. | 2026-06-21 |
| REQ-PROFILE-09 | pass (R) | `CreateChannelController` only selects the law for an already-resolved channel; FEAT-0003 adds no channel add/remove/reorder. Channel-set validation stays in FEAT-0023. | 2026-06-21 |
| REQ-PROFILE-10 | pass (R) | `docs/CONTROL_PIPELINE_MATH.md` scoped to the curve/overlay law with a law-scope note; new sibling `docs/CONTROL_PID_MATH.md` carries the PID identity (signed terms, anti-windup, gate). | 2026-06-21 |

**Spec vs. implementation deltas:**

- **REQ-PROFILE-05 — full state decouple deferred (deliberate).** The curve law is
  forward-wrapped (`CurveOverlayController::Evaluate` → `EvaluateChannel`), so its
  dynamic state stays on `ChannelState` rather than moving into the controller.
  REQ-PROFILE-05 is met functionally (worker-lifetime scope, fresh state per
  restart); the architectural decouple is a recorded partial (decision doc D2
  note, 2026-06-21). No behavioral consequence under the restart-selected scope.
- **Derivative sign.** Implemented as `+Kd·d(temp)/dt` (positive), correct for this
  feature's `error = temp − target_c` convention; the decision doc's textbook
  `−Kd·d(temp)/dt` (for `error = setpoint − measurement`) is reconciled in the D3b
  note and `docs/CONTROL_PID_MATH.md`.
- **allow_live = downgrade, not throw.** An unevidenced `pid.allow_live: true` is
  downgraded to shadow at controller construction (per D6's "stays in
  shadow/dry-run"), not a config-load failure; only malformed pid config throws.
  Config load gained a `controller`/`pid` parse + malformed-pid validation
  (section 5/7 unchanged in intent).
- **Live PID M evidence scope.** The accepted M evidence is channel-0-only and
  operator-gated. It proves the `allow_live` + characterization artifact + slew
  cap path can write live and roll back cleanly; it does not change the shipped
  default controller kind and does not overturn the all-channel PID rejection in
  `docs/pid-shadow-characterization-2026-06-21.md`.
