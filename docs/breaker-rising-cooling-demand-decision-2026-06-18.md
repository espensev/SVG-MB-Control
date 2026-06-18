# Decision: an open write-failure breaker must not block a rising cooling command (bounded rising-demand bypass)

**Project:** svg-mb-control
**Status:** Current (Accepted 2026-06-18)
**Owning feature:** `docs/features/FEAT-0011-write-failure-breaker-rising-cooling-demand.md`
(`REQ-COOLWRITE-*`)
**Companion to:** `AGENTS.md`, `docs/WRITE_ORCHESTRATION.md`,
`docs/CONTROL_PIPELINE_MATH.md`, `docs/MEASUREMENT_GATE.md`,
`docs/features/FEAT-0010-write-actuation-sidecar-fault.md`

## Context

After `kMaxConsecutiveFailures` = 5 consecutive `ApplyDuty` failures,
`HandleChannelWriteFailure` opens the per-channel breaker; while open, the gate at
`src/control/channel_write.cpp:307` suppresses every write that is not a
sensor-safe (`safety_override`) command. The breaker closes only inside
`NoteSuccessfulChannelWrite` — reached only after a **successful** `ApplyDuty`,
which a normal write never reaches while the breaker is open — or via the operator
`--reset-breakers` file. There is no half-open probe. A healthy-sensor channel
whose actuator has recovered, computing a rising (more-cooling) setpoint as
temperature climbs, is therefore trapped: the very write that would prove the
actuator works and self-heal the breaker is the one the gate blocks. Static-verified
gap; corroborated by `docs/discovery-recovery-gap-audit-2026-06-04.md` and the
2026-06-17 team review.

## Options considered

- **Hold (no change).** Rely on `--reset-breakers` + SMU/THERMTRIP backstop. Leaves
  the recovered-actuator under-cool window open.
- **Accept a rising-demand bypass.** Let a rising command on a healthy-sensor
  channel reach `ApplyDuty` while the breaker is open, bounded so it does not become
  an every-tick retry against a still-dead actuator. Bound candidates: a
  high-temperature threshold, a margin above `last_issued_pct`, a per-channel
  probe-rate limit, or a combination.

## Decision

**Accept the rising-demand bypass, bounded by a margin above `last_issued_pct`
plus a per-channel probe-rate limit.** Reasons:

- The trap is one-directional and unsafe-leaning: it blocks exactly the
  more-cooling command a hot channel needs. The sensor-safe bypass already covers
  sensor-loss; this covers the healthy-sensor recovered-actuator case the breaker
  cannot otherwise discover.
- A **margin above `last_issued_pct`** keeps futile-write suppression for
  down-or-equal commands (the breaker keeps doing its job against a still-failing
  actuator, `REQ-COOLWRITE-02`), while a **per-channel probe-rate limit** bounds how
  often a rising trial fires so the bypass cannot degenerate into an every-tick
  write against a truly-dead actuator (`REQ-COOLWRITE-04`). The pair both self-heals
  and bounds futile retries — the candidate the spec §11 already leaned toward.
- Self-heal stays observable through the existing events: a bypassed rising write
  that succeeds runs `NoteSuccessfulChannelWrite` and closes the breaker
  (`circuit_breaker_closed`); one that fails re-enters `HandleChannelWriteFailure`
  and leaves it open, with `consecutive_write_failures` neither double-counted nor
  skipped (`REQ-COOLWRITE-03`).

**Bound is fixed in code initially**, not operator-tunable (spec §11 lean); a config
surface is revisited only if tuning is requested. An unbounded every-tick bypass is
rejected — it partially defeats futile-write suppression (`REQ-COOLWRITE-04`).

## Scope and gate

- **Scope:** the breaker-gate decision at `channel_write.cpp:307` and the
  per-channel state in `control_runtime_context.h`. The computed duty, cadence,
  channel set, 5-failure open threshold, `--reset-breakers` surface, sensor-safe
  bypass, and watchdog policy are unchanged (`REQ-COOLWRITE-05`). Distinct from
  FEAT-0010 (sidecar-persist veto), which fixed a different gate.
- **Measurement gate:** not crossed (`docs/MEASUREMENT_GATE.md`); failure-path
  breaker decision only, no control-identity term added. Any new probe-rate status
  field is additive to `docs/RUNTIME_HOME.md`.
- **Implementation/verification** are authorized by this decision but are **staged
  for a Windows-host session** (Windows-only build, `CMAKE_RC_COMPILER`): the
  open-breaker rising/down-or-equal, self-heal-on-success, stay-open-on-failure, and
  bound-respected C++ tests (via `src/hardware/simulated_fan_writer.cpp`) must pass
  under `Test-LocalCI` before the spec §14 log is filled.
