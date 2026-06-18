# Decision: a source-aware channel's CPU dropout counts toward the safe-mode trip (force kSafeModeFanDuty)

**Project:** svg-mb-control
**Status:** Current (Accepted 2026-06-18)
**Owning feature:** `docs/features/FEAT-0013-source-aware-primary-dropout-safe-mode.md`
(`REQ-SRCSAFE-*`)
**Companion to:** `AGENTS.md`, `docs/CONTROL_LOOP.md`,
`docs/CONTROL_PIPELINE_MATH.md`, `docs/RUNTIME_HOME.md`, `docs/MEASUREMENT_GATE.md`

## Context

On a `TempBlend::MaxCpuGpuSourceAware` channel, when the CPU input drops to
unavailable (NaN `Tctl/Tdie`) while the GPU input remains available,
`SelectPrimaryCurveInput` returns the GPU temperature with `available = true`
(`src/control/channel_evaluator.cpp:219-234`). Because the primary reports
`available = true`, `EvaluatePrimarySetpoint` takes the recovery branch and
**resets** `consecutive_sensor_failures` to `0` (`channel_evaluator.cpp:268-276`),
so the three-miss safe-mode trip never accumulates, no `FailureDetected` event is
emitted, and `safety_override` is never set. The CPU-hot guard
(`source_aware_cpu_hot_guard_c`) also requires `cpu_available`, so it cannot fire.
A `CpuOnly` channel already trips safe mode on the same loss
(`tests/cpp/channel_write_tests.cpp:251`); the gap is specific to the
source-aware/max path. All six live channels are `max_cpu_gpu_source_aware`
(`config/control.release.json`). Team review §High; static-verified, not
runtime-reproduced. Total loss (both inputs gone) is already handled correctly.

## Options considered

Failure response after the threshold:

- **Force `kSafeModeFanDuty` + `safety_override`** (mirrors the proven `CpuOnly`
  sensor-failure response).
- **Hold a CPU-derived floor** (last good CPU-curve duty).
- **Event-only** (log the dropout, no duty change).

## Decision

**Adopt: force `kSafeModeFanDuty` + `safety_override` after the threshold,** reusing
the existing sensor-failure mechanism. Reasons:

- The channel's purpose on these blends is CPU protection; when the CPU sub-input is
  lost the controller can no longer see the thing it is protecting, so the
  conservative, cooling-safe response is to drive the fans up — exactly what the
  proven `CpuOnly` path does (`channel_evaluator.cpp:250-264`). This is the spec §11
  default.
- **Holding a CPU-derived floor is rejected:** the floor would be a stale value with
  no live CPU temperature behind it, risking under-cooling precisely during a CPU
  thermal event the controller can no longer observe.
- **Event-only is rejected:** it leaves the fan command on the GPU curve while CPU
  is unmonitored, i.e. it observes the gap without closing it.
- Reusing `kMaxConsecutiveSensorFailures` (3) and the existing `FailureDetected` /
  `Recovered` vocabulary keeps one sensor-failure state machine rather than a
  parallel one (`REQ-SRCSAFE-02`, `REQ-SRCSAFE-03`).

**Dropout vs never-present (`REQ-SRCSAFE-04`):** the trip applies only to a *loss of
a previously-available* CPU input — armed on the first observed CPU-available tick,
not on a GPU-led channel that never had CPU — which requires new per-channel state
(a `cpu_input_was_available` flag), since `last_primary_temp_source` records the
selected source, not the sub-input history.

**v1 scope:** CPU primary only; the symmetric GPU-dropout case is out of v1
(GPU loss while CPU remains already keeps CPU response). Threshold stays
`kMaxConsecutiveSensorFailures` (3), no new config key.

## Scope and gate

- **Scope:** the partial-dropout failure path in `EvaluateChannel`. With both inputs
  present, the computed duty, cadence, channel set, blend strategy, and
  control-computation identity are unchanged, and the total-loss behavior is
  unchanged (`REQ-SRCSAFE-05`).
- **Measurement gate:** not crossed (`docs/MEASUREMENT_GATE.md`); dropout-path only,
  fail-safe direction, no control-identity term moved. Any new status field is
  additive to `docs/RUNTIME_HOME.md`.
- **Implementation/verification** are authorized by this decision but are **staged
  for a Windows-host session** (Windows-only build, `CMAKE_RC_COMPILER`): the
  source-aware sibling of `channel_write_tests.cpp:251` (CPU available → dropped while
  GPU stays, three-miss trip, recovery, dropout-vs-never-present) must pass under
  `Test-LocalCI` before the spec §14 log is filled.
