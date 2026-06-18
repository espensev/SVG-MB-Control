# FEAT-0005: Write actuation confirmation (non-actuating-write detection)

**Project:** svg-mb-control
**Status:** Accepted   **Version:** 0.2   **Updated:** 2026-06-18
**Namespace:** `REQ-ACTCONFIRM-*`
**Companion to:** `AGENTS.md`, `docs/WRITE_ORCHESTRATION.md`, `docs/CONTROL_LOOP.md`, `docs/CONTROL_PIPELINE_MATH.md`, `docs/RUNTIME_HOME.md`, `docs/MEASUREMENT_GATE.md`
**Purpose:** detect a fan write that the driver accepts but that does not
actuate the fan (the chip ignores it, a co-tool overwrote it, or the motor is
stalled) — the "quiet-and-hot" case that today has no detector.

## 1. Summary

The controller currently treats a write as successful whenever the driver call
returns no error; there is no post-write confirmation that the commanded duty
produced the intended actuation (`src/control/channel_write.cpp:330-341`,
`src/hardware/fan_writer.h:26`). As a result a quiet-and-hot failure — duty
frozen low while a *defined* CPU/GPU temperature climbs — has no detector: the
write circuit breaker counts only driver-level errors
(`src/control/channel_write.cpp:44-46`), the sensor-safe 100% fallback triggers
only on *undefined* temperature (`src/control/channel_evaluator.cpp:243-260`),
and the duty-drift authority-reassert check compares only the duty *register*,
not airflow/RPM, and is suppressed during hold windows
(`src/control/channel_evaluator.cpp:119-144, 380`). This feature adds a
post-write actuation-confirmation signal that compares commanded duty against
observed fan response over a window and raises a distinct, observable condition
on sustained mismatch. Whether that condition also changes write behavior (for
example, escalating to a safe duty) is a second phase, gated by
`docs/MEASUREMENT_GATE.md` and a design decision; the minimum shippable version
is detection plus evidence only.

## 2. Problem & motivation  *(promotion gate 1)*

This is a named code/contract gap, confirmed by a code audit on 2026-06-03. The
quiet-and-hot path is reachable and silent:

1. **Write "success" is driver-error-only; no readback.** A write is treated as
   successful when `FanWriteResult.error == kNone`
   (`src/control/channel_write.cpp:330-341`; `src/hardware/fan_writer.h:26`), and
   `NoteSuccessfulChannelWrite` clears the failure counter with no verification
   that the hardware changed (`src/control/channel_write.cpp:95-120`). `ApplyDuty`
   performs no post-write read (`src/hardware/sio_fan_writer.cpp:247-260`).
2. **The breaker counts only driver errors.** It opens after 5 consecutive
   *driver* failures (`kMaxConsecutiveFailures = 5`,
   `src/control/control_runtime_context.h:78`;
   `src/control/channel_write.cpp:44-46`) and then freezes duty at
   `last_issued_pct` (`src/control/channel_write.cpp:300-301, 360`), cleared only
   by a manual runtime reset request
   (`src/control/tick_runner.cpp:36-128`). A driver-success-but-no-actuation
   write never increments the counter.
3. **Sensor-safe is blind to defined-but-hot.** The 100% fallback fires only on
   `!primary.available` (`src/control/channel_evaluator.cpp:243-260`); a defined,
   climbing temperature flows through the normal curve and never triggers it.
4. **The nearest existing mechanism does not cover this.** The duty-drift
   authority-reassert compares the duty register against `last_issued_pct` with a
   3.0% tolerance (`src/control/channel_evaluator.cpp:119-144`;
   `kAuthorityDutyTolerancePct`, `src/control/channel_evaluator.h:14`), is gated
   by `effective_hold_ms == 0u` (`src/control/channel_evaluator.cpp:380`) with a
   2000 ms cooldown (`kAuthorityReassertCooldownMs`,
   `src/control/channel_evaluator.h:13`), and never reads RPM. It catches a
   register *overwrite*, not a non-actuating fan.
5. **RPM is sampled but unused for faults.** RPM is read only for calibration
   (`src/control/calibration.cpp:192-243`) and low-band evidence statistics
   (`src/control/low_band_integrator.cpp:217-225`); it is not joined into any
   post-write or health decision.
6. **Compounding interaction.** Once the breaker is open,
   `TryApplyChannelSetpoint` returns early
   (`src/control/channel_write.cpp:300-301`), so even if temperature later goes
   undefined, the sensor-safe 100% write is also blocked.

The result is a gap between what the controller can detect and what any of its
existing reflexes can act on: a fan that is commanded high but is not moving air
(stuck pin, chip in a mode that ignores the write, or a co-tool that captured the
channel) is invisible while temperatures climb.

## 3. Goals & non-goals

**Goals**
- Add a per-channel post-write actuation-confirmation signal: compare commanded
  duty to observed fan response over a window and flag sustained non-actuation.
- Make the signal observable (status field + event + evidence fields) so the
  quiet-and-hot case is no longer silent.
- Define an optional, measurement-gated escalation path (Phase 2) for when a
  confirmed non-actuation should change behavior — as a separate, authorized
  step, not the minimum.

**Non-goals**
- The Phase-1 minimum must not change write timing, cadence, channels, breaker
  state, or control math. It is detection/evidence, mirroring `FEAT-0002`'s
  read-only posture.
- No continuous closed-loop RPM *control* (RPM setpoint tracking). Confirmation
  is a fault detector, not a controller on RPM.
- No third-party tool dependency for fan readback; uses in-repo `FanWriter`
  telemetry only.
- This feature, by itself, does not change the watchdog restart policy.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| No fan write / authority change outside an explicit live task | `AGENTS.md` §Live Runtime Safety | Phase 1 is read-only detection and emits no write. Any Phase-2 escalation that changes duty is an automatic authority action and must be a separate, explicitly authorized, evidence-gated change. |
| Shipped cadence / channel set / input strategy is the measured baseline | `docs/MEASUREMENT_GATE.md` | Phase-1 evidence logging does not change cadence, channels, or write timing and does not cross the gate. Phase 2 introduces a response-derived input that affects writes and **does** cross the gate. |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | Phase 1 adds no term to the control identity. Phase 2 would, and must be documented and validated there before implementation. |
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | Detection uses in-repo `FanWriter` telemetry; no external sensor process or sibling repo. |
| Runtime sidecar / status / manifest schema stays backward-compatible | `docs/RUNTIME_HOME.md` | New status/evidence fields and event types are additive; absence means "unknown." Existing archives stay valid. |

## 5. Behavior specification

Proposed behavior (not yet implemented). Phased: Phase 1 is detection/evidence;
Phase 2 (gated) is escalation.

- **Per-channel comparison.** After writes, compare commanded duty
  (`last_issued_pct`) against observed fan response. Candidate response sources:
  (a) per-channel duty readback, already sampled via `FanWriter::ReadAllChannels`
  in `src/platform/direct_runtime_snapshot.cpp:206`; (b) RPM, currently sampled
  only off the control path (`src/control/calibration.cpp`,
  `src/control/low_band_integrator.cpp`) — its per-tick availability on the
  control side must be confirmed before it can be used (§11).
- **Windowed, not instantaneous.** A suspect condition is raised only on a
  sustained mismatch over a defined window (N samples / T ms), so a single
  transient does not trip it.
- **No false positive at fan-stop.** Detection must flag "high duty commanded,
  response absent," not "response low." A legitimately low/zero response at a
  low/zero commanded duty (intended fan stop) must not be flagged.
- **Distinct from the reassert gate, and not hold-gated.** The detector must not
  inherit the authority-reassert hold-window blind spot
  (`src/control/channel_evaluator.cpp:380`): it should evaluate across holds. Its
  relationship to the existing authority-reassert path must be defined so the two
  do not double-fire or conflict.
- **Observable.** On sustained mismatch onset and clearance, set an additive
  per-channel status field and emit an event; record the commanded-vs-observed
  evidence so an analyzer can review it later.
- **Phase 2 (gated, separate authorization).** Whether a confirmed non-actuation
  also changes write behavior — for example forcing a safe duty, escalating
  health, or opening the breaker — is deferred to a design decision (§9) and is
  gated by `docs/MEASUREMENT_GATE.md` and `docs/CONTROL_PIPELINE_MATH.md`. The
  detector gives an independent signal that does not depend on the breaker or on
  undefined temperature, which is what closes the gap identified in §2 item 6.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-ACTCONFIRM-01 | The controller must compute a per-channel actuation-confirmation signal comparing commanded duty against observed fan response over a defined window. |
| REQ-ACTCONFIRM-02 | Detection must distinguish "high duty commanded, response absent" (suspected non-actuation) from a legitimately low/zero response at low/zero commanded duty (no false positive at fan-stop). |
| REQ-ACTCONFIRM-03 | The Phase-1 signal must be detection/evidence only: it must not change duty, cadence, channels, breaker state, or control-computation identity. |
| REQ-ACTCONFIRM-04 | The signal must be additive to the status / runtime-home schema and must emit an event on sustained-mismatch onset and clearance; existing files and schema must remain valid. |
| REQ-ACTCONFIRM-05 | Detection must not be disabled during control hold windows, and must be defined so it does not duplicate or conflict with the existing authority-reassert path. |
| REQ-ACTCONFIRM-06 | Any Phase-2 escalation that changes write behavior must be a separate, explicitly authorized change, gated by `docs/MEASUREMENT_GATE.md` and recorded in `docs/CONTROL_PIPELINE_MATH.md`. |
| REQ-ACTCONFIRM-07 | Detection must use in-repo `FanWriter` telemetry only; no third-party tool, subprocess, or sibling-repo dependency. |

## 7. Data / schema deltas

- **New per-channel status / evidence fields** (additive): a suspect flag (e.g.
  `actuation_suspect`, tri-state) and the supporting evidence (commanded duty vs
  observed response, e.g. an `actuation_gap` measure) so the condition is
  reviewable, not just a boolean.
- **New event types** `control_loop.actuation_suspected` /
  `control_loop.actuation_cleared`.
- **Possible telemetry addition:** if per-tick RPM is chosen as a response source
  and is not already in the per-channel snapshot, add it as an additive field
  (decision in §9/§11).
- **Schema/version impact:** additive only; update `docs/RUNTIME_HOME.md` (status
  fields, events) and `docs/RUNTIME_LOGGING_AND_EVALUATION.md` (evidence columns)
  at implementation. No existing archive or config becomes invalid.
- **Config impact:** window length and thresholds may need config keys; decided at
  implementation (§11).

## 8. CLI / config / operator surface deltas

- Report the per-channel actuation-confirmation state in `--diagnose` /
  `--health` (`src/app/app_diagnose.cpp`), read-only in Phase 1.
- No new operator write action in Phase 1. A Phase-2 escalation, if adopted,
  would add operator-visible behavior and is out of scope until its decision and
  measurement gate are satisfied.
- Update `README.md`, `docs/WRITE_ORCHESTRATION.md`, and `docs/RUNTIME_HOME.md`
  per `AGENTS.md` §Change Checklist. UI is out of scope (`docs/MEASUREMENT_GATE.md`).

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| `docs/actuation-confirmation-decision-2026-06-18.md` | The response source (duty readback vs RPM vs both) and its confirmed per-tick availability; the window length and mismatch thresholds relative to the shipped 250 ms cadence; and whether/what Phase-2 escalation is adopted (force safe duty / escalate health / open breaker) and how it is measurement-gated. Settled: per-tick RPM is available (`FanChannelState.rpm`/`tach_valid`); Phase-1 source is RPM gated on `tach_valid`, windowed; Phase-2 escalation deferred behind the Measurement Gate (D-ACTCONFIRM-1..5). | Current |

Decided (`docs/actuation-confirmation-decision-2026-06-18.md`): **ship Phase 1
(detection + evidence) first; defer escalation.** A read-only detector closes the
visibility half of the gap with no Live Runtime Safety or Measurement Gate
exposure, can be validated against simulated non-actuation, and produces the
evidence needed to design a safe escalation. RPM is the stronger non-actuation
signal (it sees a stalled fan that the duty register does not), and the open
question — whether control-side per-tick RPM is available — is resolved
affirmatively: `FanWriter::ReadAllChannels` returns `FanChannelState.rpm` /
`tach_valid` each tick (`src/hardware/fan_writer.h:30-43`,
`src/platform/direct_runtime_snapshot.cpp:218`). Phase-1 detection therefore uses
**RPM gated on `tach_valid`** as the primary source, with duty readback
corroborating. Escalation that changes duty is a control-affecting authority
action and must clear the Measurement Gate and `CONTROL_PIPELINE_MATH.md` on its
own.

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-ACTCONFIRM-01 | T | `.\scripts\Test-LocalCI.ps1`: simulated channel, commanded duty vs observed response over a window |
| REQ-ACTCONFIRM-02 | T | test via `src/hardware/simulated_fan_writer.cpp`: commanded-high-but-flat trips; commanded-zero-and-flat does not |
| REQ-ACTCONFIRM-03 | R | code review vs `docs/CONTROL_PIPELINE_MATH.md` / `docs/WRITE_ORCHESTRATION.md`: no duty/cadence/breaker/identity change |
| REQ-ACTCONFIRM-04 | T, M | test asserts additive fields + onset/clear events; runtime evidence |
| REQ-ACTCONFIRM-05 | T, R | test that detection evaluates during a hold window; review vs the authority-reassert path |
| REQ-ACTCONFIRM-06 | R | review confirms Phase-2 escalation is absent in Phase 1 and is gated by `docs/MEASUREMENT_GATE.md` |
| REQ-ACTCONFIRM-07 | R | code review: in-repo `FanWriter` telemetry only; no third-party/subprocess/sibling dependency |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Response source: duty readback vs RPM vs both | implementation (§9) | Confirm per-tick RPM availability; if unavailable, duty readback first, RPM as a follow-on |
| Window length and mismatch thresholds | implementation | Window measured against the 250 ms tick; thresholds chosen so a single transient does not trip |
| Whether to adopt Phase-2 escalation, and which action | a separate, gated decision | Detection-only first; escalation deferred behind the Measurement Gate |
| Whether to add per-tick RPM to the channel snapshot | implementation | Add only if RPM is the chosen source and is not already present |

## 12. Measurement gate & dependencies

- **Measurement gate:** Phase 1 (detection/evidence) is read-only and does not
  cross `docs/MEASUREMENT_GATE.md`. Phase 2 (escalation) introduces a
  response-derived input that affects writes and **does** cross the gate; it
  requires the characterization evidence named there and a
  `docs/CONTROL_PIPELINE_MATH.md` update before implementation.
- **Depends on:** existing per-tick `FanWriter::ReadAllChannels` telemetry
  (`src/platform/direct_runtime_snapshot.cpp:206`) and the channel-write path
  (`src/control/channel_write.cpp`). Can consume the `FEAT-0004` hardware-access
  signal to avoid flagging non-actuation when the write path is known unavailable.
- **Build/test impact:** new tests under `tests/` driven by
  `src/hardware/simulated_fan_writer.cpp`; doc updates per `AGENTS.md` §Change
  Checklist. Phase 2 only: `CONTROL_PIPELINE_MATH.md` update.

## 13. Promotion-gate checklist

- [x] 1. Problem stated as a named code/contract gap with file:line evidence (§2).
- [x] 2. Stressed invariants identified — Live Runtime Safety, Measurement Gate, control identity, Repo Boundary, RUNTIME_HOME schema (§4).
- [x] 3. Required design decision record written and marked current (§9 — `docs/actuation-confirmation-decision-2026-06-18.md`, Current).
- [x] 4. Concrete `REQ-ACTCONFIRM-*` IDs assigned (§6).
- [x] 5. Verification mapped to `Test-LocalCI` / review / runtime evidence (§10).
- [x] 6. Confirmed Phase 1 does not violate Live Runtime Safety or Repo Boundary and does not move the Measurement Gate baseline; Phase 2 is explicitly gated rather than bundled in.
- [x] 7. Doctrine check: current behavior claims grounded with file:line; proposed behavior labeled as proposed; `must`/`should`/`is` used per `CLAUDE.md`.

> Accepted 2026-06-18: un-parked (body restored to the enforced set) and promoted
> Reserved → Accepted; the §9 design decision record
> (`docs/actuation-confirmation-decision-2026-06-18.md`) is written and Current,
> closing gate 3. Accepted scope is Phase-1 detection/evidence only; Phase-2
> escalation is a separate, measurement-gated authorization. Accepted is not
> itself build authorization.

## 14. Verification log  *(fill in after the feature is built)*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-ACTCONFIRM-01 | | | |
| REQ-ACTCONFIRM-02 | | | |
| REQ-ACTCONFIRM-03 | | | |
| REQ-ACTCONFIRM-04 | | | |
| REQ-ACTCONFIRM-05 | | | |
| REQ-ACTCONFIRM-06 | | | |
| REQ-ACTCONFIRM-07 | | | |

**Spec vs. implementation deltas:** <record anything built differently from this
spec, and why. Update §5/§6 and the cited contract docs if behavior changes, and
bump **Updated**.>
