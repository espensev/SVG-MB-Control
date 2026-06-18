# Decision: Write actuation confirmation — Phase-1 detection source and scope

**Project:** svg-mb-control
**Status:** Current
**Owns:** FEAT-0005 §9 (`REQ-ACTCONFIRM-*`) — promotion gate 3.
**Basis:** the 2026-06-04 recovery-gap audit
(`docs/discovery-recovery-gap-audit-2026-06-04.md`, Q5 and Q6 remediation 1) and
FEAT-0005 §2 (the named code/contract gap with file:line evidence). Per-tick RPM
availability verified against source (`src/hardware/fan_writer.h:30-43`,
`src/platform/direct_runtime_snapshot.cpp:212-224`).

## 1. Context

The controller treats a write as successful whenever the driver call returns no
error; there is no post-write confirmation that the commanded duty produced
actuation (`src/control/channel_write.cpp:330-341`, `src/hardware/fan_writer.h:26`).
A "quiet-and-hot" failure — a fan commanded high while a defined temperature
climbs but the fan does not move air (stalled motor, a chip mode that ignores the
write, or a co-tool that captured the channel) — has no detector: the breaker
counts only driver errors, sensor-safe fires only on *undefined* temperature, and
the authority-reassert check compares the duty *register*, not airflow
(`docs/discovery-recovery-gap-audit-2026-06-04.md` Q5). RPM is sampled but no
control decision consumes it.

This decision settles FEAT-0005 §9: the response source and its confirmed per-tick
availability, the detection shape, and the Phase-1/Phase-2 boundary.

## 2. Decisions

- **D-ACTCONFIRM-1 — per-tick RPM is available on the control-side snapshot
  (the spec's open question, resolved).** `FanWriter::ReadAllChannels` returns a
  `FanChannelState` per fan that already carries `rpm`, `tach_raw`, and
  `tach_valid` (`src/hardware/fan_writer.h:30-43`), and the control loop already
  calls it each tick (`src/platform/direct_runtime_snapshot.cpp:218`). No schema
  addition is required to *read* RPM for the detector.
- **D-ACTCONFIRM-2 — the Phase-1 actuation-confirmation source is RPM, gated on
  `tach_valid`.** RPM is the strong actuation-truth signal: it detects a stalled
  fan, which a duty-register readback cannot (the register can read back the
  commanded value while no air moves). Duty readback (`FanChannelState.duty_percent`)
  corroborates but is not the primary signal. A channel without a valid tach
  reports unknown, never suspected.
- **D-ACTCONFIRM-3 — windowed detection; no false positive at fan-stop.** A
  suspect condition is raised only on a *sustained* mismatch over a defined window
  (N samples / T ms): commanded duty at or above an activation threshold while
  observed RPM is at or near zero with `tach_valid`. A legitimately low/zero RPM
  at a low/zero commanded duty (an intended fan stop) must not be flagged. Exact
  window length and thresholds are chosen at implementation against the shipped
  250 ms tick (FEAT-0005 §11).
- **D-ACTCONFIRM-4 — evaluate across hold windows; distinct from the
  authority-reassert path.** The detector must not inherit the authority-reassert
  hold-window blind spot (`src/control/channel_evaluator.cpp:380`); it evaluates
  during holds. It is RPM-based where the authority-reassert is duty-register-based,
  and the two must not double-fire.
- **D-ACTCONFIRM-5 — Phase 1 is detection/evidence only; Phase-2 escalation is
  deferred.** Phase 1 adds an additive per-channel status field and onset/clear
  events and changes no duty, cadence, channel, breaker state, or control
  identity. Whether a confirmed non-actuation also changes write behavior (force a
  safe duty, escalate health, or open the breaker) is a separate, explicitly
  authorized change gated by `docs/MEASUREMENT_GATE.md` and recorded in
  `docs/CONTROL_PIPELINE_MATH.md` (REQ-ACTCONFIRM-06).

## 3. Risks & mitigations

- **Spin-up transient false positive.** Mitigation: the sustained-window
  requirement (D-ACTCONFIRM-3) absorbs spin-up lag; a single transient does not
  trip the signal.
- **Fanless or tach-less channel.** Mitigation: detection is gated on
  `tach_valid`; such a channel reports unknown, never suspected.
- **Double-firing with authority-reassert.** Mitigation: D-ACTCONFIRM-4 requires
  the relationship be defined so the two paths do not conflict; the detector is
  RPM-based and not hold-gated.

## 4. Rollback

Phase 1 is an additive status field + events fed by already-sampled telemetry, so
removal is dropping the field/events and reverting the `docs/RUNTIME_HOME.md`
delta; no control behavior depends on it. Phase 2 is not in scope here.
