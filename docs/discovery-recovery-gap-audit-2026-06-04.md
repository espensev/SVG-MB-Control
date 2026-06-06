# Discovery - Automatic Recovery Gap Audit

**Goal:** Map every automatic recovery actor in the runtime, determine which
fault classes each one can act on, and identify fault classes that are detected
but left unrecovered - including any compound failure where the fail-safe
philosophy inverts to fail-silent.

**Date:** 2026-06-04
**Status:** complete; findings verified against source. Remediation 3 (break
the compound cell) implemented 2026-06-06 with a regression test; remediations 1
and 2 remain open. The Q1-Q5 findings below describe the pre-fix behavior.
**Recommended next:** treat this as the evidence record for three remediations
(actuation-truth signal, degraded/failed escalation wiring, compound-cell
break). Any code change to the control or recovery path is evidence-gated per
`docs/CONTROL_PIPELINE_MATH.md` and `docs/COOLING_STRATEGY.md`, and must respect
the Live Runtime Safety rules in `AGENTS.md`.

This is a discovery/risk record, not a normative reference. Per `AGENTS.md`,
treat `docs/discovery-*.md` as historical context unless it is confirmed current
against README, the maintained docs, source, and tests.

---

## Questions

1. Which automatic recovery actors exist, and what exactly triggers each?
2. How does a runtime fault map to a health exit code, and which actor (if any)
   can see that code?
3. Which fault classes are detected but reach no recovery actor?
4. Is there a compound failure where the fail-loud safe path is silently
   suppressed?
5. Is there any actuation-truth signal - does the system ever confirm that a fan
   physically responded to a write?
6. What remediation closes the gaps, and where in the code does each change land?

---

## Findings

### Q1: Which automatic recovery actors exist, and what triggers each?

**Answer:** There are exactly two automatic recovery actors, and each reacts to
only one signal. The supervisor reacts to a **process exit**. The watchdog
reacts to a **health exit code of 2 only**. Neither reads the per-channel
hardware-fault flags.

**Evidence:**
- `src/control/control_supervisor.cpp:550` - the supervisor restart loop runs
  `while (!RuntimeStopRequested(runtime_home))`; it only acts when the worker
  process exits and `WaitForSingleObject` returns.
- `src/control/control_supervisor.cpp:638` - the loop breaks (stops restarting)
  iff `stop_requested || exit_code == 0u`; otherwise it restarts. So the restart
  trigger is "non-zero process exit while no stop was requested" - a crash, not
  a health state.
- `src/control/control_supervisor.cpp:647` - backoff is
  `min(60, 1 << min(restart_count, 5))` seconds, i.e. 2, 4, 8, 16, 32, then held
  at 32 (the 60 cap never binds because `1 << 5 == 32`).
- `src/control/control_supervisor.cpp:618` - the only fast-fail guard is narrow:
  it returns without restart **only** when `restart_count == 0 &&
  exited_during_startup && !stop_requested && exit_code != 0`, where
  `exited_during_startup` means the worker exited inside the 1500 ms window
  (`kWorkerStartupCheckMs`, `src/control/control_supervisor.cpp:576`). A startup
  failure that takes longer than 1.5 s to exit, or any crash on a later
  iteration, is restarted with backoff.
- `src/platform/task_runner.cpp:195-205` - the `--watchdog-run` branch runs
  `--health --json`, then: health `0` or `1` returns `0` (no-op, reported as the
  watchdog's own success); health `2` runs `--restart`; any other code
  (`3` and the error codes) returns the health value and does not restart.
- The watchdog file does not implement periodicity; Task Scheduler drives the
  cadence.

**Implications:**
- The supervisor belongs to the supervised lifecycle (`--start` spawns
  `--run-supervisor`); `--run-foreground` bypasses it entirely.
- A worker that stays alive but reports trouble triggers neither actor: it never
  exits (so the supervisor sees no crash) and it reports health `1` (so the
  watchdog returns its own success).

### Q2: How does a fault map to a health exit code, and which actor can see it?

**Answer:** `AssessHealthState` is a fixed-order cascade. Live hardware faults on
a still-running worker all land on **degraded -> exit code 1**, which is exactly
the one code the watchdog treats as success.

**Evidence:**
- `src/runtime/runtime_health.cpp:75-177` - `AssessHealthState` early-returns in
  this order: status file absent -> `kStopped`; status JSON unreadable ->
  `kFailed`; pending-writes sidecar unreadable -> `kFailed`; status in the
  not-active set -> `kFailed`; `shutdown` -> `kStopped`; process not active ->
  `kStopped`; stale flag -> `kStale`; unparseable last-update -> `kStale`;
  last-update older than the staleness threshold -> `kStale`; stop request
  present -> `kDegraded`; **`degraded_channel_count > 0` -> `kDegraded`
  (`:163`)**; status not `running` -> `kDegraded`; else -> `kHealthy`.
- `src/runtime/runtime_health.cpp:219-232` - `RuntimeHealthExitCode`:
  healthy `0`, degraded `1`, stale `2`, stopped `2`, failed `3`.
- `src/runtime/runtime_status.cpp:71-80` - `DegradedChannelCount` counts a
  channel when `circuit_breaker_open || sensor_failed ||
  consecutive_write_failures > 0`. A channel is therefore degraded after a
  **single** write failure, before the breaker (threshold 5) even trips.

**Implications:**
- The degraded-channel check sits **after** every terminal and stale check, so a
  channel-level hardware fault surfaces as exit `1` only when nothing
  terminal/stale fired first.
- The exit-code mapping is the seam: stale/stopped (`2`) is the only code the
  watchdog restarts on, and process crash is the only event the supervisor
  restarts on. Degraded (`1`) and failed (`3`) trigger no automatic recovery.

### Q3: Which fault classes are detected but reach no recovery actor?

**Answer:** Every live hardware-path fault. They are detected (flags are set,
events are logged, the channel counts as degraded) but they map to exit `1`,
which neither actor acts on.

**Evidence:**
- Write path dead mid-run: `src/control/channel_write.cpp:42-47` -
  `consecutive_write_failures` increments and the breaker opens at
  `kMaxConsecutiveFailures == 5` (`src/control/control_runtime_context.h:78`).
  Result: degraded (`1`).
- Temp path dead: `src/control/channel_evaluator.cpp:248-259` - after
  `kMaxConsecutiveSensorFailures == 3` (`src/control/control_runtime_context.h:82`)
  the channel sets `sensor_failed` and targets `kSafeModeFanDuty == 100.0`
  (`src/control/control_runtime_context.h:83`). Result: degraded (`1`).
- PawnIO/driver absent **mid-run**: writes fail -> breaker opens after 5 ->
  degraded (`1`); the worker stays alive and unrecovered.
- The only fully covered case is "process stops publishing", which becomes
  stale/stopped (`2`) and is the one code the watchdog restarts on.

**Implications:**
- Degraded is a terminal resting state for hardware faults: the fan is parked
  (see Q4) and nothing escalates or pages.

### Q4: Is there a compound failure where the fail-loud safe path is suppressed?

**Answer:** Yes, and it is confirmed in code. A channel whose write-failure
breaker is already open, that then loses its temperature sensor, computes the
sensor-safe 100% command and has it silently dropped by the breaker gate. The
one place the system is designed to fail loud is exactly where it fails silent.

**Evidence:**
- `src/control/channel_evaluator.cpp:248-259` - the sensor-safe path sets
  `raw_desired_setpoint = kSafeModeFanDuty` and `response_source =
  "sensor_safe_mode"` with **no breaker awareness**; the setpoint is produced
  normally (`has_setpoint` becomes true).
- `src/control/channel_write.cpp:300-302` - the write path returns early when
  `channel.circuit_breaker_open`, and this gate runs **before** the actuation
  call.
- `src/control/channel_write.cpp:330-331` - `ApplyDuty` is the actual write, and
  it is reached only past the breaker gate. The full gate order is: cooldown
  (`:287`), `baseline_captured` (`:294`), `RuntimeFanAllowsWrite` (`:297`),
  `circuit_breaker_open` (`:300`).
- Breaker clearing is one-way while open: `NoteSuccessfulChannelWrite`
  (`src/control/channel_write.cpp:95-120`) closes the breaker only after a
  successful `ApplyDuty`, which the `:300` gate makes unreachable while the
  breaker is open. The breaker therefore only clears via an explicit reset
  request (`src/control/tick_runner.cpp:71-77`) or a process restart
  (`ChannelState` defaults `circuit_breaker_open = false`).

**Implications:**
- The breaker exists to stop futile writes after repeated failures. A
  thermal-safety write is precisely the write that should not be suppressed, so
  the breaker and the safe path are in direct conflict in this cell.
- This is the highest-severity, smallest-surface finding.

### Q5: Is there any actuation-truth signal?

**Answer:** No. RPM is read and logged, but no control decision ever consumes it.
The system cannot tell that a fan physically responded to a write; a fan stalled
at a correct-looking commanded duty is invisible to the control path.

**Evidence:**
- RPM is read into the runtime snapshot and emitted to CSV/evidence and
  accumulated into calibration/low-band statistics only.
- The four core control modules - `src/control/channel_evaluator.cpp`,
  `src/control/channel_write.cpp`, `src/control/tick_runner.cpp`,
  `src/control/boost_stage.cpp` - reference `rpm` zero times in their control
  decisions.
- Authority-reassert detection (`DetectAuthorityReassert`) compares duty and
  mode drift only; it never reads RPM.
- There is no over-temperature kill switch: a defined-but-hot temperature tops
  out at the curve's highest duty (`curve.back().duty_pct`), clamped to
  `[floor, 100]`, plus bounded boosts. The only 100% path is the sensor-failed
  safe mode in Q4.

**Implications:**
- "Write succeeded" means the driver call returned, not that airflow changed.
  This is the one fault class that no current detector covers at all.

### Q6: What remediation closes the gaps, and where does each change land?

**Answer:** Three changes, in payoff order. All touch the control/recovery path
and are evidence-gated; none were applied.

1. **Actuation-truth signal (the missing sensor).** After a write settles,
   compare `fan.rpm` against an expectation for the commanded duty and escalate
   on divergence. Lands near `DetectAuthorityReassert` in
   `src/control/channel_evaluator.cpp`, plus a new `ChannelState` flag wired into
   `DegradedChannelCount` (`src/runtime/runtime_status.cpp:71-80`). Closes the
   only undetected fault class (Q5).
2. **Escalate degraded and failed instead of swallowing them.** Driver absence
   never produces a futile restart loop - the startup fast-fail guard returns it
   without restarting, and mid-run loss degrades rather than crashes - so the gap
   is missing escalation, not bad restart behavior. The watchdog reports health
   `1` as its own success and propagates health `3` without alerting
   (`src/platform/task_runner.cpp:195-205`); add an alert/page branch so a
   degraded or failed channel reaches an operator. No supervisor change is
   required for the driver-absence case.
3. **Break the compound cell.** Let a sensor-safe setpoint bypass the
   write-failure breaker (`src/control/channel_write.cpp:300-302`), or fail the
   channel to a hardware-safe state rather than a frozen duty. Smallest change,
   highest safety value. **Implemented 2026-06-06:** a `safety_override` flag on
   `ChannelEvaluation` (set on the safe-mode path in `channel_evaluator.cpp`)
   lets `TryApplyChannelSetpoint` pass the breaker gate for a sensor-safe
   command only; a successful bypassed write closes the breaker, a failed one
   leaves it open and logs `control_loop.write_failed`. Covered by
   `tests/cpp/channel_write_tests.cpp`.

**Evidence:** see Q1-Q5 for the exact lines each remediation targets.

**Implications:**
- Remediation 3 changes write-gating behavior and must be covered by a test that
  proves a sensor-safe command reaches `ApplyDuty` past an open breaker before it
  ships.
- Remediations 1 and 2 change health/escalation semantics and require
  `docs/RUNTIME_HOME.md` and `docs/RUNTIME_LOGGING_AND_EVALUATION.md` updates per
  the `AGENTS.md` change checklist.

---

## Cross-Cutting Analysis

### Recovery gap matrix

| Fault | Detection (control_runtime.json) | Health exit | Watchdog | Supervisor | Outcome |
|---|---|---:|---|---|---|
| Write path dead (mid-run) | `circuit_breaker_open` after 5 failures; `consecutive_write_failures` counts from 1 | 1 | no-op (returns 0) | no crash | Fan parked at last issued duty; unrecovered |
| Temp path dead | `sensor_failed` after 3 failures; `last_response_source=sensor_safe_mode` | 1 | no-op | no crash | Ramps toward 100% - unless breaker already open (see compound cell) |
| PawnIO/driver absent at startup | `status=failed` then worker exits 1 | 3 (leftover file) | propagates, no restart | no restart (fast-fail guard) | `--start` surfaces the error; not auto-restarted |
| PawnIO/driver absent mid-run | breaker opens -> degraded | 1 | no-op | no crash | Same as write-path-dead |
| Fan stalled at correct duty | none (RPM is telemetry only) | 0 | no-op | no crash | Undetected |
| Process stops publishing | stale status file | 2 | restarts | restarts if crashed | Recovered (the only covered case) |

### The compound cell

`channel_evaluator.cpp:248-259` computes the sensor-safe 100% setpoint with no
breaker awareness; `channel_write.cpp:300-302` drops any write when the breaker
is open, before `ApplyDuty` at `:330`. So an already-breaker-open channel that
then loses its sensor produces the safe-loud command and has it silently
suppressed. No bypass path exists, and the breaker cannot self-clear while open.

### Note on the startup driver-absence case

The startup driver-absence case is handled by the supervisor's fast-fail guard,
not by a restart loop. When the writer cannot be constructed at worker startup
(for example PawnIO/driver absent), the control loop catches the failure, writes
`status=failed`, and returns 1 before the tick loop runs and before the `running`
status is written (`src/control/control_loop.cpp:90-107`). The worker therefore
exits non-zero on its first iteration, inside the 1500 ms startup window - which
is exactly the condition the guard at `src/control/control_supervisor.cpp:618`
tests (`restart_count == 0 && exited_during_startup && !stop_requested &&
exit_code != 0`). The guard returns the exit code **without restarting**, and the
`--start` launcher surfaces the error to the operator. There is no futile startup
restart loop.

Driver absence also does not produce a restart loop mid-run: writes fail, the
breaker opens, and the worker stays alive in degraded state (exit `1`) rather
than crashing, so the supervisor never sees a crash to restart. The failed
(exit `3`) state is consequently only observed from a leftover `failed` status
file after the worker has already exited, or from corrupt status/sidecar JSON -
never from a live worker.

The load-bearing rule is therefore simple: every fault on a still-running worker
lands on degraded (exit `1`), which is inert to both the watchdog and the
supervisor. The gap is the absence of escalation on that state, not bad restart
behavior. The supervisor does restart on a genuine mid-run crash (a non-zero exit
after the 1.5 s window, or on a later iteration), which is the intended behavior.

### Risks

| Risk | Likelihood | Impact | Notes |
|---|---:|---:|---|
| Compound cell suppresses the sensor-safe 100% command on a hot channel | Low | High | Requires breaker already open when the sensor drops; both are single-channel hardware faults. Remediation 3. |
| Stalled fan at a correct commanded duty goes unnoticed | Medium | High | No actuation-truth signal exists. Remediation 1. |
| Degraded or failed channel never reaches an operator | Medium | Medium | Watchdog reports health `1` as success and propagates `3` without paging; no actor escalates. Remediation 2. |

### Open questions

- Remediation 1 needs an RPM-vs-duty expectation model (per-fan floor RPM, or a
  calibration-derived band). The calibration statistics already collected may
  supply it, but no model is specified or tested yet.
- The audit verified detection and recovery wiring against source; it did not run
  a live fault-injection trace. A read-only runtime trace under induced write
  failure would confirm the observed health/event sequence before any change.

---

## Recommendation

Adopt this as the evidence record for the recovery gap. If a single slice is
selected first, take remediation 3 (break the compound cell): it is the smallest
surface and the highest safety value, and it is testable by asserting a
sensor-safe command reaches `ApplyDuty` past an open breaker. Do not apply any of
the three changes as a "cleanup" - each alters control or recovery semantics and
is gated by the checks in `docs/CONTROL_PIPELINE_MATH.md`,
`docs/COOLING_STRATEGY.md`, and the `AGENTS.md` change checklist.
