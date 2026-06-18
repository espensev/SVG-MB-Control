# FEAT-0013: Source-aware channels enter safe mode on primary-source dropout

**Project:** svg-mb-control
**Status:** Done   **Version:** 0.2   **Updated:** 2026-06-18
**Namespace:** `REQ-SRCSAFE-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/CONTROL_LOOP.md`, `docs/CONTROL_PIPELINE_MATH.md`
**Purpose:** propose that a source-aware (max-blend) channel whose previously-available
CPU input drops out be treated as a partial sensor failure that counts toward the
existing sensor-failure safe-mode trip, so a CPU thermal event on a blended channel is
not left unmonitored while the GPU input keeps the channel out of safe mode.

## 1. Summary

On a channel running `TempBlend::MaxCpuGpuSourceAware`, every CPU-protective path is
gated on the CPU input being available
(`temp_inputs.cpu_available`). When the CPU input drops to unavailable (NaN
`Tctl/Tdie`) while the GPU input is still available, the source-aware selector returns
the GPU temperature with `available = true`
(`src/control/channel_evaluator.cpp:219-234`). Because the primary selection reports
`available = true`, `EvaluatePrimarySetpoint` takes the recovery branch and **resets**
`consecutive_sensor_failures` to `0`
(`src/control/channel_evaluator.cpp:268-276`), so the three-consecutive-miss safe-mode
trip (`channel_evaluator.cpp:250-264`, `kMaxConsecutiveSensorFailures = 3`) never
accumulates, no `ChannelSensorEvent::FailureDetected` is emitted, and
`safety_override` is never set. The CPU-hot guard that was explicitly designed to force
CPU heat onto these channels (`source_aware_cpu_hot_guard_c`,
`channel_evaluator.cpp:219-224`) cannot fire either, because it also requires
`cpu_available`. This feature proposes counting a CPU-input dropout on such a channel
toward the existing sensor-failure trip; it is held at Draft pending a maintainer
decision on the failure response. A `CpuOnly` channel already trips safe mode on the
same loss (`TestEvaluatorSetsSafetyOverrideOnSensorFailure`,
`tests/cpp/channel_write_tests.cpp:251`); the gap is specific to the source-aware/max
path.

## 2. Problem & motivation  *(promotion gate 1)*

This is a named code/contract gap, static-verified against
`src/control/channel_evaluator.cpp` on 2026-06-17 and recorded as a High fail-direction
finding (source-aware CPU-dropout) in
`review/svg-mb-control-review-20260617-team-review.md` §High. It is statically verified,
not yet runtime-reproduced.

1. **The source-aware selector reports a GPU fallback as an available primary.**
   `SelectPrimaryCurveInput` for `TempBlend::MaxCpuGpuSourceAware`
   (`src/control/channel_evaluator.cpp:219-234`): the hot-guard branch at line 220
   requires `inputs.cpu_available` to even compare CPU against
   `source_aware_cpu_hot_guard_c`, so a CPU dropout (`cpu_available == false`) skips it;
   control then falls to the GPU branch at line 225, which sets `available = true` and
   `source = "gpu"` whenever `inputs.gpu_available`.
2. **An available primary resets the sensor-failure counter.** Back in
   `EvaluatePrimarySetpoint` (`src/control/channel_evaluator.cpp:268-276`), `primary.available`
   is `true`, so the recovery branch runs and sets `consecutive_sensor_failures = 0`.
   The safe-mode trip at lines 250-264 — the path that sets
   `s.raw_desired_setpoint = ChannelState::kSafeModeFanDuty` (100%) and
   `s.evaluation.safety_override = true` — is only reached when `!primary.available`, so
   it never fires on a CPU-only dropout while the GPU input is present.
3. **The CPU-override curve is bypassed too.** `ApplyCpuOverride`
   (`src/control/channel_evaluator.cpp:287-301`) returns early when
   `!s.temp_inputs.cpu_available`, so the per-channel CPU-override curve cannot
   re-introduce CPU response on the dropout path either.
4. **The result is silent.** No `ChannelSensorEvent::FailureDetected` is emitted for
   the missing CPU sub-input, `safety_override` stays unset, and the channel commands
   the GPU-curve setpoint (plus any GPU-driven boost). The fail direction is
   stuck-low-against-an-unmonitored-CPU rather than fail-to-zero, but it removes the CPU
   protection the channel was configured to provide.

**Reachability is production-relevant.** All six live channels are
`max_cpu_gpu_source_aware` with `source_aware_cpu_hot_guard_c: 75.0`
(`config/control.release.json`); no `CpuOnly` channel exists. The trigger is a
sustained CPU-only NaN on the controller's `Tctl/Tdie` composite (a PawnIO read drop or
AMD module hiccup) while the GPU reader succeeds and the GPU is cool. (The per-CCD Tdie
glitch named in the original finder text is a misattribution — that glitch feeds
`cpu_max_c`, not the controller's `Tctl/Tdie` composite per
`MEMORY.md` cpu-peak-temp-108c — so it is **not** the trigger; the valid triggers are
the read-drop / module-hiccup classes.)

**Bounded honestly.** This is a **partial** sensor-loss gap: total loss (both
`cpu_available` and `gpu_available` false) is already handled — `SelectPrimaryCurveInput`
returns `available = false`, so `EvaluatePrimarySetpoint` accumulates the trip and
enters safe mode after three misses. The gap is only the case where one sub-input
(CPU) drops while the other (GPU) remains. Hardware backstops (the SMU 95 °C throttle)
remain in effect and are outside the controller; this feature concerns the controller's
own response, not whether the silicon protects itself.

## 3. Goals & non-goals

**Goals**
- Make a source-aware/max-blend channel treat the loss of a previously-available CPU
  input as a partial sensor failure that counts toward the existing
  `consecutive_sensor_failures` trip rather than being masked by the GPU fallback.
- Produce an operator-visible signal (a sensor-failure event) when a source-aware
  channel's CPU sub-input drops out, where today there is none.
- Confine the change to the dropout failure path: normal-operation computed duty,
  cadence, channel set, and blend strategy stay unchanged.

**Non-goals**
- No change to the blend math, curve lookup, boost overlays, or rate limiting on the
  normal (both-inputs-present) path.
- No change to the already-correct total-loss behavior (both inputs unavailable already
  trips safe mode).
- No change to `CpuOnly` / `GpuOnly` / legacy `MaxCpuGpu` behavior beyond what the
  shared sensor-failure mechanism already provides.
- Does not address the GPU-input dropout symmetric case beyond what §11 records as an
  open decision; v1 scope is the CPU primary on a source-aware channel.
- Does not change hardware backstops, the watchdog, or the write-failure breaker.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| No fan write / authority change outside an explicit live task | `AGENTS.md` §Live Runtime Safety | The control loop already owns the write; this changes only the per-channel sensor-failure state machine inside `EvaluateChannel`, which feeds the existing write path. It adds no new write site and no new live action. |
| Shipped 250 ms cadence / channel set / input strategy is the measured baseline | `docs/MEASUREMENT_GATE.md` | Dropout-path only. Normal-operation cadence, channels, and blend strategy are unchanged, so the gate baseline does not move. The dropout response moves fan output in the fail-safe direction (proposed). |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | The both-inputs-present computed duty is identical; only the partial-dropout response changes. The change is to the sensor-failure transition, not to a blend/boost term. |
| Runtime sidecar / status / manifest schema stays backward-compatible | `docs/RUNTIME_HOME.md` | Any new field is additive (a per-channel partial-dropout indicator) and reuses the existing `ChannelSensorEvent` vocabulary; absence reads as "unknown." Existing archives stay valid. |
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | The change is confined to in-repo control code (`src/control/channel_evaluator.cpp`, `src/control/control_runtime_context.h`); no external dependency. |

## 5. Behavior specification

Implemented behavior. It lives in `EvaluateChannel`'s
primary-selection step (`src/control/channel_evaluator.cpp`,
`SelectPrimaryCurveInput` / `EvaluatePrimarySetpoint`) and the per-channel state in
`src/control/control_runtime_context.h` (`ChannelState`).

- **Normal operation unchanged.** When both inputs are present, or when CPU was never
  present at startup and the channel is GPU-led by configuration, the existing
  selection and curve behavior is unchanged.
- **CPU dropout counts toward the trip (proposed).** When a source-aware channel had a
  CPU input available on a prior tick and the CPU input becomes unavailable while GPU
  remains available, the evaluation must treat that tick as a sensor miss for the
  purpose of `consecutive_sensor_failures` — it must **not** reset the counter to `0`
  on the strength of the GPU fallback alone.
- **Fail-safe response after the threshold.** After
  `ChannelState::kMaxConsecutiveSensorFailures` (3) consecutive CPU-dropout ticks, the
  channel enters the existing sensor-safe state (the exact response is an open decision,
  §11: force `kSafeModeFanDuty` + `safety_override`, or hold a CPU-derived floor). The
  reused mechanism is the one already proven for `CpuOnly`
  (`channel_evaluator.cpp:250-264`).
- **Recovery clears the condition.** When the CPU input returns, the channel clears the
  partial-failure state and resumes normal source-aware selection, emitting
  `ChannelSensorEvent::Recovered` exactly as the existing full-loss recovery path does
  (`channel_evaluator.cpp:268-276`).
- **Observable, not silent.** A CPU dropout that crosses the threshold must emit a
  sensor-failure event so `--status` / the event log records it, where today the
  channel logs nothing.
- **Dropout vs never-present.** The trip applies to a *loss of a previously-available*
  CPU input, not to a channel that is GPU-only by configuration and never had CPU. This
  requires per-channel state to remember that CPU was available
  (`ChannelState` carries `last_primary_temp_source` today, but that records the
  *selected* source, not the CPU sub-input availability history, so new state is
  required — see §7).

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-SRCSAFE-01 | On a `TempBlend::MaxCpuGpuSourceAware` channel whose CPU input was available on a prior evaluation and is now unavailable while the GPU input remains available, the evaluation must count the tick toward `consecutive_sensor_failures` and must not reset that counter on the GPU fallback alone. |
| REQ-SRCSAFE-02 | After `ChannelState::kMaxConsecutiveSensorFailures` (3) consecutive CPU-dropout ticks, the channel must enter the sensor-safe response (the chosen response from §11; default: set `safety_override` and command `kSafeModeFanDuty`), reusing the existing sensor-failure mechanism rather than a parallel one. |
| REQ-SRCSAFE-03 | A CPU-dropout-driven safe-mode entry must emit a sensor-failure event (`ChannelSensorEvent::FailureDetected`), and a subsequent CPU-input return must emit `ChannelSensorEvent::Recovered` and restore normal source-aware selection. |
| REQ-SRCSAFE-04 | The trip must distinguish a CPU *dropout* (CPU previously available, now unavailable) from a CPU *never-present* configuration (GPU-led channel that never had CPU): a never-present CPU input on a GPU-available channel must not trip the CPU-dropout safe mode. |
| REQ-SRCSAFE-05 | The change must be confined to the dropout failure path: with both inputs present, the computed duty, cadence, channel set, blend strategy, and control-computation identity are unchanged, the total-loss behavior is unchanged, and any new status field is additive to `docs/RUNTIME_HOME.md`. |

IDs come from this feature's `REQ-SRCSAFE-*` namespace, reserved in the registry in
`docs/features/README.md`. Keep them stable once published.

## 7. Data / schema deltas

- New/changed fields: per-channel state to record that a CPU sub-input was available on
  a prior tick (e.g. a `cpu_input_was_available` boolean and a dedicated
  `consecutive_cpu_dropout_misses` counter, or reuse of `consecutive_sensor_failures`
  with a guard flag) in `ChannelState`
  (`src/control/control_runtime_context.h`). The existing
  `last_primary_temp_source` records the *selected* source string, not the CPU
  sub-input availability, so it cannot by itself distinguish dropout from
  never-present — new state is required. No new `ChannelSensorEvent` enumerator is
  required; the existing `FailureDetected` / `Recovered` vocabulary is reused.
- Config impact (`config/control.*.json`, `config/machines/*.json`): none required;
  the trip reuses `kMaxConsecutiveSensorFailures`. Whether the threshold becomes a
  per-channel config key is an open decision (§11).
- Schema/version impact: additive only at implementation; any status field exposing the
  partial-dropout condition is optional and degrades to "unknown" when absent. Update
  `docs/RUNTIME_HOME.md` (status field + the new sensor-event trigger) and
  `docs/CONTROL_LOOP.md` (the source-aware dropout behavior) at implementation. No
  existing runtime-home file, archive, or config becomes invalid.

## 8. CLI / config / operator surface deltas

- `--status` reflects the new sensor-failure event and, if added, the additive
  per-channel partial-dropout indicator (read-only). No new operator write action.
- No new CLI subcommand or flag in v1. UI is out of scope (`docs/MEASUREMENT_GATE.md`).
- Update `README.md` only if the `--status` field list it documents changes; otherwise
  the doc updates are `docs/RUNTIME_HOME.md` and `docs/CONTROL_LOOP.md` per `AGENTS.md`
  §Change Checklist.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/source-aware-cpu-dropout-decision-2026-06-17.md`](../source-aware-cpu-dropout-decision-2026-06-17.md) | The failure response (force `kSafeModeFanDuty` + `safety_override`, D-SRCSAFE-1), scope (`MaxCpuGpuSourceAware` + CPU-primary only, D-SRCSAFE-2), counter/threshold reuse (D-SRCSAFE-3), and the dropout-vs-never-present rule (D-SRCSAFE-4). | Current (settled 2026-06-17) |

The decision record was settled and the maintainer authorized implementation
2026-06-17; the §11 defaults were adopted as the v1 design.

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-SRCSAFE-01 | T | C++ test (a source-aware sibling of `tests/cpp/channel_write_tests.cpp:251` `TestEvaluatorSetsSafetyOverrideOnSensorFailure`): feed a source-aware channel CPU-available then drop CPU while GPU stays available; assert `consecutive_sensor_failures` increments rather than resetting. |
| REQ-SRCSAFE-02 | T | C++ test: three consecutive CPU-dropout ticks set `safety_override` and command `kSafeModeFanDuty` on a `max_cpu_gpu_source_aware` channel. |
| REQ-SRCSAFE-03 | T, R | C++ test asserts `ChannelSensorEvent::FailureDetected` on the dropout trip and `ChannelSensorEvent::Recovered` when CPU returns; review vs `docs/RUNTIME_HOME.md` event list. |
| REQ-SRCSAFE-04 | T | C++ test: a GPU-led channel that never had a CPU input does not trip the CPU-dropout safe mode, while a channel that had CPU and lost it does. |
| REQ-SRCSAFE-05 | R | Review vs `docs/CONTROL_PIPELINE_MATH.md` and `docs/MEASUREMENT_GATE.md`: both-inputs-present computed duty/cadence/channels/identity and total-loss behavior unchanged; new status field additive. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

**Resolved 2026-06-17** by `docs/source-aware-cpu-dropout-decision-2026-06-17.md`;
the defaults below were adopted as the v1 design.

| Decision | Needed before | Current default |
|---|---|---|
| The failure response after the threshold: force `kSafeModeFanDuty` + `safety_override` (mirrors `CpuOnly`), hold a CPU-derived floor, or event-only. | the design decision record | Force `kSafeModeFanDuty` + `safety_override`, mirroring the existing `CpuOnly` sensor-failure response, because the channel's purpose is CPU protection. |
| Whether the symmetric GPU-input dropout on a source-aware channel is in scope (GPU loss while CPU remains already keeps CPU response, so it may be lower priority). | implementation | Out of v1 scope; CPU primary only. |
| Whether the dropout threshold is a per-channel config key or stays `kMaxConsecutiveSensorFailures` (3). | implementation | Reuse `kMaxConsecutiveSensorFailures` (3); no new config key in v1. |
| The exact dropout-vs-never-present rule (require N prior consecutive CPU-available ticks before arming the trip). | implementation | Arm on the first observed CPU-available tick; disarm only on explicit GPU-only configuration. |

## 12. Measurement gate & dependencies

- **Measurement gate:** not crossed. The change is dropout-path only; it does not change
  normal-operation cadence, live channels, or mixed-input strategy, and adds no term to
  the control identity, so no characterization evidence is required before
  implementation (`docs/MEASUREMENT_GATE.md`). The dropout response moves fan output in
  the fail-safe (toward-100% or hold-floor) direction.
- **Depends on:** the existing channel-evaluation path
  (`src/control/channel_evaluator.cpp`) and per-channel state
  (`src/control/control_runtime_context.h`). Cross-references the source-aware
  configuration of the live channels (`config/control.release.json`) but adds no new
  dependency.
- **Build/test impact:** new C++ tests under `tests/cpp/` driven by `EvaluateChannel`
  with an injected `TempInputs` sequence (CPU available → dropped → returned); doc
  updates to `docs/RUNTIME_HOME.md` and `docs/CONTROL_LOOP.md` per `AGENTS.md`
  §Change Checklist. No `docs/CONTROL_PIPELINE_MATH.md` math change.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2 — static-verified `channel_evaluator.cpp:219-234,268-276,287-301`; team-review §High source-aware CPU-dropout).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [x] 3. Required design decision record(s) written and marked current (§9 — `docs/source-aware-cpu-dropout-decision-2026-06-17.md`, Current).
- [x] 4. Concrete `REQ-SRCSAFE-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — `Test-LocalCI` C++ tests and contract review (§10), and mirrored in `docs/TRACEABILITY.md`.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline (dropout-path only; normal-operation computed duty unchanged; fail-safe direction; additive schema).
- [x] 7. Doctrine check: current behavior claims grounded with file:line; proposed behavior labeled as proposed; `must`/`should`/`is` used per `CLAUDE.md`; no undefined terms or unqualified vague adjectives.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-SRCSAFE-01 | pass | `tests/cpp/channel_write_tests.cpp::TestSourceAwareCpuDropoutCountsTowardTrip` (a CPU dropout increments `consecutive_sensor_failures` instead of resetting on the GPU fallback) — CTest green | 2026-06-17 |
| REQ-SRCSAFE-02 | pass | `channel_write_tests.cpp::TestSourceAwareCpuDropoutTripsSafeMode` (3 dropout ticks set `safety_override` + response source `source_aware_cpu_dropout_safe_mode`) — CTest green | 2026-06-17 |
| REQ-SRCSAFE-03 | pass | `...TripsSafeMode` asserts `ChannelSensorEvent::FailureDetected` on the trip; `...CpuRecoveryClearsDropout` asserts `Recovered` on CPU return — CTest green | 2026-06-17 |
| REQ-SRCSAFE-04 | pass | `channel_write_tests.cpp::TestSourceAwareNeverPresentCpuDoesNotTrip` (GPU-led, CPU never seen → no trip) — CTest green | 2026-06-17 |
| REQ-SRCSAFE-05 | pass | `channel_write_tests.cpp::TestSourceAwareBothPresentNoTrip` (both-inputs-present unchanged); change confined to the dropout branch in `EvaluatePrimarySetpoint`; total-loss path unchanged | 2026-06-17 |

**Spec vs. implementation deltas:** Implemented per the decision record. Reuses the
existing `consecutive_sensor_failures` counter, `kMaxConsecutiveSensorFailures`
threshold, `sensor_failed` health degradation, and `FailureDetected`/`Recovered`
events (D-SRCSAFE-3); the only new state is the additive `cpu_input_was_available`
flag. The dropout trip carries response source `source_aware_cpu_dropout_safe_mode`;
below the threshold the channel keeps cooling on the GPU curve. Scope is
`MaxCpuGpuSourceAware` + CPU-primary only (D-SRCSAFE-2); REQ-SRCSAFE-01 dropped the
"(or MaxCpuGpu)" qualifier accordingly.
