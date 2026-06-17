# Source-aware CPU-dropout safe mode — Decision & Plan — 2026-06-17

**Status:** Current — decision settled 2026-06-17. Settles FEAT-0013 promotion
gate 3 and the §11 open decisions; authorizes the v1 implementation.
**Owns:** `docs/features/FEAT-0013-source-aware-primary-dropout-safe-mode.md`
(`REQ-SRCSAFE-*`).
**Basis:** the static-verified gap in `src/control/channel_evaluator.cpp`
(`SelectPrimaryCurveInput` returns the GPU fallback with `available=true` on a CPU
dropout, so `EvaluatePrimarySetpoint` resets `consecutive_sensor_failures` and the
3-miss safe-mode trip never fires); corroborated by the 2026-06-17 external review
(Finding 1). All six live channels are `max_cpu_gpu_source_aware`
(`config/control.release.json`), so the gap is production-reachable.

## 1. Context

On a `TempBlend::MaxCpuGpuSourceAware` channel a CPU-input dropout while the GPU
input remains available is silently masked: the GPU fallback keeps the channel out
of safe mode, the CPU-hot guard cannot fire (it needs `cpu_available`), and the
CPU-override curve is bypassed (`ApplyCpuOverride` returns early when CPU is
unavailable). A `CpuOnly` channel already trips safe mode on the same loss; the gap
is specific to the source-aware/max path.

## 2. Decisions

### D-SRCSAFE-1 — Failure response: reuse the CpuOnly safe-mode trip
After the threshold, a confirmed CPU dropout sets `safety_override` and commands
`ChannelState::kSafeModeFanDuty` (100%), reusing the exact mechanism the `CpuOnly`
sensor-failure path uses, with a distinct response source
`source_aware_cpu_dropout_safe_mode` for diagnostics.

*Rationale:* the channel's purpose is CPU protection; the proven mechanism degrades
health (`sensor_failed`), bypasses the write-failure breaker, and emits the
existing `ChannelSensorEvent::FailureDetected` / `Recovered` events — no parallel
state machine. Rejected: hold a CPU-derived floor (no live CPU reading to derive
from during a dropout) and event-only (leaves the CPU thermally unmonitored).

### D-SRCSAFE-2 — Scope: `MaxCpuGpuSourceAware` only; CPU primary only
v1 covers `TempBlend::MaxCpuGpuSourceAware`. Legacy `MaxCpuGpu` is **out of scope**
(it is unused by the live single-profile config), and the symmetric GPU-input
dropout is out of scope (GPU loss while CPU remains keeps CPU response, so it is
lower priority).

*Rationale:* matches the live risk and the spec §3 non-goals. This resolves the
spec self-consistency nit the review flagged (REQ-SRCSAFE-01 dropped the
"(or MaxCpuGpu)" qualifier).

### D-SRCSAFE-3 — Reuse `consecutive_sensor_failures` and the threshold (3)
The dropout counts toward the existing `consecutive_sensor_failures` counter and
the existing `kMaxConsecutiveSensorFailures = 3` threshold; no parallel counter and
no new config key. Below the threshold the channel keeps cooling on the available
(GPU) curve rather than going dark.

*Rationale:* REQ-SRCSAFE-01/02 require reuse of the existing mechanism. The only new
per-channel state is a single `cpu_input_was_available` flag.

### D-SRCSAFE-4 — Dropout vs never-present: arm on first CPU-available tick
The trip applies only after CPU has been observed available at least once
(`cpu_input_was_available`). A GPU-led channel that never had CPU never arms the
trip.

*Rationale:* REQ-SRCSAFE-04. `last_primary_temp_source` records the *selected*
source, not the CPU sub-input history, so a dedicated flag is required.

## 3. Risks & mitigations
- **False trip on a brief CPU read glitch.** Mitigated by the 3-consecutive-tick
  threshold (≈750 ms at the 250 ms cadence) before safe mode; a single missed read
  keeps the GPU curve and does not trip.
- **Behavior change on the normal path.** None: with both inputs present the
  selection, curve, boosts, and rate-limiting are byte-identical; only the
  CPU-dropout transition changes (REQ-SRCSAFE-05).
- **Total-loss regression.** None: both-inputs-unavailable still takes the existing
  `!primary.available` trip, unchanged.

## 4. Rollback
Contained: revert the `EvaluatePrimarySetpoint` branch and the additive
`cpu_input_was_available` field. No schema, config, or runtime-home migration.
