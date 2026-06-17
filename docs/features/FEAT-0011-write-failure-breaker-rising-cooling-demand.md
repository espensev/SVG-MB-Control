# FEAT-0011: Write-failure breaker must not block rising cooling demand

**Project:** svg-mb-control
**Status:** Implemented   **Version:** 0.2   **Updated:** 2026-06-17
**Namespace:** `REQ-COOLWRITE-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/WRITE_ORCHESTRATION.md`, `docs/features/FEAT-0010-write-actuation-sidecar-fault.md`
**Purpose:** investigate, and propose a maintainer-decidable direction for, the
case where the write-failure circuit breaker — opened by transient `ApplyDuty`
failures — keeps suppressing a *rising* (more-cooling) command after the actuator
has recovered, so the breaker cannot self-heal in the cooling direction and the
channel stays frozen at a possibly-low duty while temperature climbs.

## 1. Summary

After `kMaxConsecutiveFailures` = 5 consecutive `ApplyDuty` failures on a channel
(`src/control/channel_write.cpp:44-47`,
`src/control/control_runtime_context.h:78`), `HandleChannelWriteFailure` sets
`channel.circuit_breaker_open = true`. While the breaker is open, the gate at
`src/control/channel_write.cpp:307`
(`if (channel.circuit_breaker_open && !evaluation.safety_override) return;`)
suppresses every write that is not a sensor-safe command. The breaker is cleared
in only two places: `NoteSuccessfulChannelWrite`
(`src/control/channel_write.cpp:95-120`), which runs **only after a successful
`ApplyDuty`** — but a normal write never reaches `ApplyDuty` while the breaker is
open — and the operator `--reset-breakers` file request applied in
`tick_runner.cpp:77`. There is no half-open probe: the code in
`channel_write.cpp` contains no `half_open`, `probe`, or automatic-retry path.

The consequence is a one-way trap in the cooling direction. The `safety_override`
bypass at line 307 covers only the sensor-loss path (`safety_override` is set
exactly once, on sustained sensor failure, `channel_evaluator.cpp:263`). A
**healthy-sensor** channel whose actuator has since recovered, computing a
rising-duty setpoint as temperature climbs, is blocked: the write that would
discover the actuator works — and would, on success, run
`NoteSuccessfulChannelWrite` and close the breaker — is the very write the gate
suppresses. The channel stays parked at its last issued (possibly low) duty until
either a sensor-safe trip or a manual `--reset-breakers`.

This spec structures that hazard and proposes one direction — a bounded
rising-demand bypass that lets a more-cooling write reach the actuator so a
recovered breaker can self-heal in the cooling direction. **It does not authorize
code and does not assert the fix is decided**; the bound and the trade against
futile-write suppression are left as a maintainer decision (§11).

## 2. Problem & motivation  *(promotion gate 1)*

This is a named code/contract gap, **static-verified against source** (no runtime
repro harness; corroborated by team-review finding 5,
`review/svg-mb-control-review-20260617-team-review.md`, and the project's own
recovery-gap audit, `docs/discovery-recovery-gap-audit-2026-06-04.md`). It was
investigated; the current behavior is described below, and promotion is proposed
only if the recovered-actuator under-cool case is judged material (§11).

1. **The open breaker suppresses all non-safety writes, including rising ones.**
   `TryApplyChannelSetpoint` reaches `src/control/channel_write.cpp:307` after the
   deadband, cooldown, baseline, and runtime-allow gates. When
   `channel.circuit_breaker_open` is `true` and `evaluation.safety_override` is
   `false`, it returns before `pending_store.Upsert` and `fan_writer.ApplyDuty`
   (`channel_write.cpp:311-348`). The returned write may be a rising one: the gate
   does not compare `evaluation.setpoint_pct` against `channel.last_issued_pct`, so
   a more-cooling command is suppressed exactly like a less-cooling one.

2. **The only self-heal path is a successful write, which the gate prevents.**
   The breaker closes inside `NoteSuccessfulChannelWrite`
   (`channel_write.cpp:104-119`), reached only after `ApplyDuty` returns success
   (`channel_write.cpp:347-356`). Because the open breaker blocks the normal write
   before `ApplyDuty`, a channel whose actuator has recovered cannot demonstrate
   recovery through a normal cooling command — there is no half-open or timed
   retry in `channel_write.cpp` to issue a trial write. The breaker therefore
   clears only via a sensor-safe trip (which needs sensor loss,
   `channel_evaluator.cpp:263`) or the operator `--reset-breakers` file
   (`tick_runner.cpp:77`).

3. **Fail direction: under-cool of a hot channel whose actuator has recovered.**
   The recovery-gap audit records this cell as a fan "parked at last issued duty"
   that is "unrecovered" (`docs/discovery-recovery-gap-audit-2026-06-04.md`,
   §"breaker" cell, lines 153-162). If the actuator is **still failing**, the open
   breaker is correct — it stops futile writes — and no harm beyond the existing
   degraded state results. The genuine gap is the **recovered-actuator** case: the
   writes are no longer futile, they would succeed and cool, but the breaker has no
   mechanism to discover that and keeps the channel frozen at a possibly-low duty
   as temperature rises.

**Why this is "partial," not a clean defect.** The breaker exists to stop futile
writes to a still-failing actuator (`docs/discovery-recovery-gap-audit-2026-06-04.md`
lines 160-162); suppression is the *intended* behavior while the actuator is dead.
The trap is real only in the recovered-actuator window, and three mitigations bound
its severity: the open requires 5 consecutive `ApplyDuty` failures (rarely reached
in this repo's recorded operation); `--reset-breakers` gives the operator a manual
exit; and AMD SMU throttling / THERMTRIP backstop the worst-case thermal envelope.
The proposal is therefore framed as a **direction to decide**, not a settled fix.

## 3. Goals & non-goals

**Goals**
- Make it possible, if the maintainer accepts the direction, for a **rising**
  (more-cooling) command on a healthy-sensor channel to reach the actuator while
  the write-failure breaker is open, so a recovered breaker can self-heal in the
  cooling direction.
- Preserve futile-write suppression for down-or-equal commands while the breaker
  is open (the breaker keeps doing its job against a still-failing actuator).
- Keep the self-heal observable: a bypassed rising write that succeeds must close
  the breaker through the existing `NoteSuccessfulChannelWrite` event, and a
  bypassed write that fails must leave the breaker open and loud (re-enter the
  existing failure path).

**Non-goals**
- No change to the computed duty, cadence, channel set, curve, overlays, or
  mixed-input strategy. Only the breaker-gate decision at
  `channel_write.cpp:307` changes.
- No change to the sensor-safe (`safety_override`) bypass, which already reaches
  the actuator (`channel_write.cpp:307`); that path is settled by recovery-gap
  remediation 3 and FEAT-0010, not re-litigated here.
- Does not change the 5-failure open threshold, the `--reset-breakers` operator
  surface, or the watchdog restart policy.
- Does not address the sidecar-persist veto (FEAT-0010, `REQ-WRITESAFE-*`) or the
  restore/reconcile blocked-channel-guard finding; those are separate rounds,
  cross-referenced in §12.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this proposal stays inside it |
|---|---|---|
| No fan write / authority change outside an explicit live task | `AGENTS.md` §Live Runtime Safety | The control loop already owns this write. The proposal changes only whether an *already-computed* rising write is applied when the breaker is open; it adds no new write site and no new live action. |
| Shipped 250 ms cadence / channel set / input strategy is the measured baseline | `docs/MEASUREMENT_GATE.md` | Breaker-gate decision only; cadence, channels, and input strategy are unchanged, so the gate baseline does not move. |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | The computed duty is identical; only whether a rising duty is applied through an open breaker changes. No control-identity term is added or altered. |
| Runtime sidecar / status / manifest schema stays backward-compatible | `docs/RUNTIME_HOME.md` | If a bounded-probe counter or event is added, it is additive and reuses the existing breaker-event vocabulary; absence reads as "unknown." Existing archives stay valid. |
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | The change is confined to in-repo control code (`src/control/channel_write.cpp` and the per-channel state in `control_runtime_context.h`); no external dependency. |

## 5. Behavior specification

Implemented behavior (per the decision record; Direction = bounded half-open
probe). It lives at the breaker gate in `src/control/channel_write.cpp`
(`TryApplyChannelSetpoint`, line 307) and the per-channel state in
`src/control/control_runtime_context.h`.

- **Down-or-equal command, breaker open — unchanged.** When the breaker is open and
  the computed setpoint is not above `channel.last_issued_pct` (no more cooling
  wanted), the gate keeps suppressing the write (futile-write suppression
  preserved). The sensor-safe (`safety_override`) bypass is unchanged — it reaches
  the actuator immediately, above the probe logic.
- **Rising command, breaker open — bounded half-open probe.** When the breaker is
  open and the computed setpoint exceeds `channel.last_issued_pct` (more cooling
  wanted) **and** at least `kBreakerProbeBackoff` (5 s) has elapsed since the last
  probe, the controller lets that one write proceed to `ApplyDuty` and records the
  probe tick (`channel.last_probe_time`).
- **Self-heal on success.** A successful probe runs the existing
  `NoteSuccessfulChannelWrite`, which closes the breaker, resets
  `consecutive_write_failures`, and emits `control_loop.circuit_breaker_closed`. The
  channel resumes normal control — the self-heal the open breaker previously
  prevented.
- **Stay loud on failure.** A failed probe runs the existing
  `HandleChannelWriteFailure`: the actuator is confirmed still-failing and the
  breaker stays open. Because `last_probe_time` is set before the write attempt, the
  next probe waits the full 5 s backoff, so a persistently-failing actuator is
  retried at most once per backoff — not every tick.
- **Observable.** The probe attempt emits an additive
  `control_loop.circuit_breaker_probe` event (best-effort — wrapped so a logging
  throw cannot veto the probe write, consistent with REQ-WRITESAFE-06); the outcome
  is the existing `circuit_breaker_closed` / `write_failed` event. The only new
  per-channel state is the internal `last_probe_time` (not published to
  `control_runtime.json`).

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-COOLWRITE-01 | While the write-failure breaker is open on a healthy-sensor channel, a computed **rising** (more-cooling) setpoint that meets the accepted bound must be allowed to reach `fan_writer.ApplyDuty`, so a recovered actuator can be exercised in the cooling direction. |
| REQ-COOLWRITE-02 | A **down-or-equal** computed setpoint must continue to be suppressed while the breaker is open: futile-write suppression for non-rising commands is preserved, and the sensor-safe (`safety_override`) bypass behavior is unchanged. |
| REQ-COOLWRITE-03 | A bypassed rising write that succeeds must close the breaker through the existing `NoteSuccessfulChannelWrite` path (event `control_loop.circuit_breaker_closed`), and one that fails must re-enter `HandleChannelWriteFailure` leaving the breaker open; neither path may double-count or skip `consecutive_write_failures`. |
| REQ-COOLWRITE-04 | The rising-demand bypass must be bounded so it does not become an every-tick write against a still-failing actuator: the accepted bound (high-temperature threshold, margin above `last_issued_pct`, and/or a probe-rate limit) is recorded in §11 and the design decision before implementation. |
| REQ-COOLWRITE-05 | The change must be confined to the breaker-gate decision: the computed duty, cadence, channel set, and control-computation identity are unchanged, and any new status/state field is additive to `docs/RUNTIME_HOME.md`. |

## 7. Data / schema deltas

- New/changed fields: at most an additive per-channel field if a probe-rate bound
  is chosen (e.g. a `last_breaker_probe_tick`, `uint64`, default `0`, optional in
  status output). If the bound is a pure threshold (no rate limit), no new field is
  required. The existing `circuit_breaker_open` status field
  (`src/runtime/runtime_status.cpp:65`) is unchanged.
- Config impact (`config/control.*.json`, `config/machines/*.json`): none unless
  the accepted bound is operator-tunable; if so, an additive key with a
  backward-compatible absent-key default. Recorded as an open decision (§11).
- Schema/version impact: additive only; update `docs/RUNTIME_HOME.md` (any new
  status field) and `docs/WRITE_ORCHESTRATION.md` (the breaker self-heal behavior)
  at implementation. No existing runtime-home file, archive, or config becomes
  invalid.

## 8. CLI / config / operator surface deltas

- No new CLI subcommand or flag is proposed. The existing `--reset-breakers`
  operator path (`src/app/app_main.cpp:164`, `tick_runner.cpp:77`) stays as the
  manual breaker exit.
- `--status` continues to report `circuit_breaker_open` per channel; if a
  probe-rate field is added it appears there (read-only). UI is out of scope
  (`docs/MEASUREMENT_GATE.md`).
- Doc updates at implementation are `docs/RUNTIME_HOME.md` and
  `docs/WRITE_ORCHESTRATION.md` per `AGENTS.md` §Change Checklist; update
  `README.md` only if a `--status` field it documents changes.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/breaker-probe-decision-2026-06-17.md`](../breaker-probe-decision-2026-06-17.md) | Accept the bounded half-open probe (D-COOLWRITE-1); fixed 5 s hardcoded `kBreakerProbeBackoff` (D-COOLWRITE-2); "cooling wanted" = setpoint above the last applied duty (D-COOLWRITE-3); reuse the existing success/failure paths, `safety_override` unchanged, additive `control_loop.circuit_breaker_probe` event (D-COOLWRITE-4). | Current (settled 2026-06-17) |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-COOLWRITE-01 | T | `.\scripts\Test-LocalCI.ps1` C++ test: an open-breaker channel with a healthy sensor and a rising computed setpoint (via `src/hardware/simulated_fan_writer.cpp`) asserts `ApplyDuty` fires for the rising command. |
| REQ-COOLWRITE-02 | T | C++ test: an open-breaker channel with a down-or-equal setpoint asserts the write is still suppressed; a separate case asserts the `safety_override` bypass is unchanged. |
| REQ-COOLWRITE-03 | T, R | C++ test: a bypassed rising write that succeeds closes the breaker (`circuit_breaker_closed`) and one that fails leaves it open with `consecutive_write_failures` advancing correctly; review vs `docs/WRITE_ORCHESTRATION.md` breaker self-heal. |
| REQ-COOLWRITE-04 | T, R | C++ test: with the accepted bound, the bypass does not fire every tick against a persistently-failing actuator (rate/threshold respected); review vs the design decision recording the bound. |
| REQ-COOLWRITE-05 | R | Review vs `docs/CONTROL_PIPELINE_MATH.md` and `docs/MEASUREMENT_GATE.md`: computed duty/cadence/channels/identity unchanged; any new status field additive to `docs/RUNTIME_HOME.md`. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

**Resolved 2026-06-17** by `docs/breaker-probe-decision-2026-06-17.md`
(D-COOLWRITE-1..4 adopted: bounded half-open probe, 5 s backoff, cooling-wanted =
setpoint above last applied duty, fixed in code).

| Decision | Needed before | Current default |
|---|---|---|
| Whether to accept the rising-demand-bypass direction at all, or hold the current behavior (operator `--reset-breakers` only) as acceptable. | promotion | Hold; the current behavior is the shipped behavior until a maintainer judges the recovered-actuator under-cool material. |
| The bound that distinguishes a "rising" bypass from a normal write: a high-temperature threshold, a margin above `channel.last_issued_pct`, a probe-rate limit, or a combination. | implementation | Undecided. A margin-above-`last_issued_pct` combined with a per-channel probe-rate limit is the candidate that both self-heals and bounds futile retries, but it is not chosen. |
| Whether an unbounded rising bypass (every-tick trial) is acceptable, given it partially defeats futile-write suppression by retrying each tick against a truly-dead actuator. | implementation | Not acceptable unbounded; a rate or threshold bound is required (REQ-COOLWRITE-04). |
| Whether the bound is operator-tunable via config or fixed in code. | implementation | Fixed in code initially (no config surface), revisited only if operator tuning is requested. |

## 12. Measurement gate & dependencies

- **Measurement gate:** not crossed. The proposal is a failure-path/breaker-gate
  decision only; it does not change cadence, live channels, or mixed-input
  strategy, and adds no term to the control identity, so no characterization
  evidence is required before a decision (`docs/MEASUREMENT_GATE.md`).
- **Depends on:** the channel-write path (`src/control/channel_write.cpp`,
  `TryApplyChannelSetpoint` and the breaker helpers) and the per-channel state
  (`src/control/control_runtime_context.h`). It is the "separate later round"
  that `docs/features/FEAT-0010-write-actuation-sidecar-fault.md` §3/§12 defers
  (`channel_write.cpp:300-309`); FEAT-0010 fixed the sidecar-`Upsert` veto and
  relies on the existing `safety_override` bypass at line 307, while this feature
  addresses the healthy-sensor rising-demand path and the absent self-heal — the
  two are distinct gates. Cross-references but does not depend on the
  restore/reconcile blocked-channel-guard finding.
- **Build/test impact:** new C++ tests under `tests/cpp/` driven by
  `src/hardware/simulated_fan_writer.cpp` (open-breaker rising vs. down-or-equal,
  self-heal-on-success, stay-open-on-failure, bound respected); doc updates to
  `docs/RUNTIME_HOME.md` and `docs/WRITE_ORCHESTRATION.md` per `AGENTS.md`
  §Change Checklist. No `docs/CONTROL_PIPELINE_MATH.md` change.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2 — static-verified code gap at `channel_write.cpp:307`, corroborated by team-review finding 5 and the recovery-gap audit; not runtime-reproduced).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [x] 3. Required design decision record(s) written and marked current (§9 — `docs/breaker-probe-decision-2026-06-17.md`, Current; bounded half-open probe).
- [x] 4. Concrete `REQ-COOLWRITE-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — `Test-LocalCI`, contract review (§10), to be mirrored in `docs/TRACEABILITY.md` on acceptance.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline (breaker-gate decision only; computed duty unchanged; additive schema).
- [x] 7. Doctrine check: current behavior claims grounded with file:line; proposed behavior labeled as proposed; `must`/`should`/`is` used per `CLAUDE.md`; no undefined terms or unqualified vague adjectives.

> Implemented 2026-06-17: the maintainer authorized the bounded half-open probe
> (decision `docs/breaker-probe-decision-2026-06-17.md`). The breaker gate now
> probes once per 5 s backoff on rising cooling demand; the former "all normal
> writes suppressed" leg of the breaker test was narrowed to a down-demand case.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-COOLWRITE-01 | pass | `channel_write_tests.cpp::TestRisingDemandProbesOpenBreaker` (a rising cooling demand probes through the open breaker to `ApplyDuty`) — CTest green | 2026-06-17 |
| REQ-COOLWRITE-02 | pass | `channel_write_tests.cpp::TestNonCoolingWriteSuppressedByOpenBreaker` (a lower setpoint stays suppressed) + `TestSensorSafeBypassesOpenBreaker` (`safety_override` bypass unchanged) — CTest green | 2026-06-17 |
| REQ-COOLWRITE-03 | pass | `TestProbeSuccessClosesBreaker` (success → breaker closed + `consecutive_write_failures` reset) + `TestProbeFailureKeepsBreakerOpen` (failure → breaker stays open) — CTest green | 2026-06-17 |
| REQ-COOLWRITE-04 | pass | `TestProbeRateLimitedWithinBackoff` (a second rising write within the 5 s `kBreakerProbeBackoff` does not probe again) — CTest green | 2026-06-17 |
| REQ-COOLWRITE-05 | pass | Review (R): change confined to the breaker gate in `channel_write.cpp`; computed duty/cadence/channels/control identity unchanged; the only new per-channel state (`last_probe_time`) is internal and the `circuit_breaker_probe` event is additive — no new published status field | 2026-06-17 |

**Spec vs. implementation deltas:** Implemented as a bounded half-open probe. The
bound is a fixed 5 s `kBreakerProbeBackoff` rate-limit (D-COOLWRITE-2) rather than a
high-temperature threshold; "cooling wanted" is `setpoint > last_issued_pct`
(D-COOLWRITE-3), generalizing the spec's "rising" framing to also cover a
hot-but-steady channel above its last applied duty. No new published status field
(REQ-COOLWRITE-05) — `last_probe_time` is internal; the additive
`control_loop.circuit_breaker_probe` event marks the probe.
