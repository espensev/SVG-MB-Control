# FEAT-0010: Write actuation survives a sidecar-persistence fault

**Project:** svg-mb-control
**Status:** Implemented   **Version:** 0.3   **Updated:** 2026-06-17
**Namespace:** `REQ-WRITESAFE-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/WRITE_ORCHESTRATION.md`
**Purpose:** a failure to persist the pending-write sidecar must not stop the
controller from commanding the fan, so a file-level fault cannot silently veto
thermal actuation (including the sensor-safe command).

## 1. Summary

On the control hot path the controller persists a pending-write sidecar entry
before it commands a fan duty. Before this fix a failure to persist that entry
returned early and skipped the fan write
(`src/control/channel_write.cpp` `TryApplyChannelSetpoint`), so a persistent fault on
`pending_writes.json` (an antivirus scan, backup snapshot, file indexer, a stuck
concurrent writer, or a disk/ACL error) freezes the fan at its last duty while
temperature rises — and suppresses the 100% sensor-safe command on the same
path. This feature makes the fan write proceed even when the sidecar persist
fails, and surfaces the persist failure as a degraded health condition instead
of a silent skip. The captured-baseline crash-recovery record is preserved on
the common path.

## 2. Problem & motivation  *(promotion gate 1)*

This is a named code/contract gap, runtime-reproduced on 2026-06-17 (finding H1,
`review/svg-mb-control-review-20260617-team-review.md`; isolated repro harness
`review/repro/h1_sidecar_veto_repro.py`; evidence
`review/repro/h1_repro_evidence.json`).

1. **The sidecar persist gates the fan write.** `TryApplyChannelSetpoint` calls
   `pending_store.Upsert(entry)` inside a `try` and `return`s from the `catch`
   before `fan_writer.ApplyDuty` (`src/control/channel_write.cpp:319-338`).
   `PendingWritesStore::Upsert` persists synchronously through the throwing
   `WriteJsonFileAtomic` (`src/runtime/pending_writes.cpp:121-141`); the atomic
   replace throws after six retries on a held target
   (`src/runtime/json_io.cpp:33-58, 187-255`).
2. **The skip is fail-silent with no backstop.** The early `return` never
   reaches `HandleChannelWriteFailure` (`src/control/channel_write.cpp:337-344`),
   so `consecutive_write_failures` does not increment and the write-failure
   breaker never escalates; `control_runtime.json` is written by the non-throwing
   `TryWriteJsonFileAtomic` (`src/runtime/runtime_status.cpp:190`), so it stays
   fresh and the staleness watchdog (`src/runtime/runtime_health.cpp`) does not
   recycle.
3. **The sensor-safe command is suppressed too.** The `safety_override` breaker
   bypass at `src/control/channel_write.cpp:307` is upstream of the Upsert gate
   at line 320, and the `catch` has no `safety_override` exemption, so a
   persistent sidecar fault suppresses the 100% safe-mode write as well.

Runtime evidence (same rising-temperature ramp, simulated hardware): with
`pending_writes.json` locked, the applied duty froze at 58.75% while the computed
setpoint climbed to 92% at 89 C; `control_loop.write_applied` stopped at 10,
`control_loop.sidecar_upsert_failed` reached 14, `circuit_breaker_open` stayed
`false`, `consecutive_write_failures` stayed `0`, and the loop advanced 10 ticks
without recycling. Without the lock the same ramp drove the duty to 100% over 30
writes.

The current write order is deliberate: persisting intent before acting lets a
crash mid-write leave a recovery record from which the next worker start restores
the captured baseline (`src/runtime/pending_writes.cpp:136-139`,
`docs/WRITE_ORCHESTRATION.md` Runtime Flow step 6). The fix must keep that
crash-recovery guarantee on the common path while removing the sidecar's veto
over actuation.

## 3. Goals & non-goals

**Goals**
- A pending-write sidecar persist failure must not prevent the computed fan duty
  from being applied.
- The sensor-safe (`safety_override`) command must reach the actuator regardless
  of sidecar persist success.
- A persist-failure-then-successful-actuation must be observable (degraded health
  + event + counter), not a silent skip.
- Preserve the persist-before-act crash-recovery record on the common path.

**Non-goals**
- No change to the computed duty, cadence, channel set, curve, or mixed-input
  strategy. Only the failure-path control flow changes.
- No new actuation when there was none before; the feature changes whether an
  already-computed write is applied on a sidecar fault, not what is computed.
- Does not address the neighboring write-path findings (open breaker blocking
  rising-demand writes; restore/reconcile bypassing the blocked-channel guard);
  those are cross-referenced (§12) and out of scope here.
- Does not change the watchdog restart policy.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| No fan write / authority change outside an explicit live task | `AGENTS.md` §Live Runtime Safety | The control loop already owns the write; this changes only whether an already-decided write is applied when the sidecar persist fails. It adds no new write site and no new live action. |
| Shipped 250 ms cadence / channel set / input strategy is the measured baseline | `docs/MEASUREMENT_GATE.md` | Failure-path only; cadence, channels, and input strategy are unchanged, so the gate baseline does not move. |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | The computed duty is identical; only its application on a sidecar fault changes. No control-identity term is added or altered. |
| Runtime sidecar / status / manifest schema stays backward-compatible | `docs/RUNTIME_HOME.md` | The backstop adds an additive per-channel counter and reuses the existing health-state vocabulary; absence of the field reads as "unknown." Existing archives stay valid. |
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | The change is confined to in-repo control/runtime code; no external dependency. |

## 5. Behavior specification

Implemented behavior (v1 plus the v0.3 REQ-WRITESAFE-06 hardening pass). It lives in or near
`src/control/channel_write.cpp` (`TryApplyChannelSetpoint`),
`src/runtime/pending_writes.cpp` (`PendingWritesStore`),
`src/control/control_runtime_context.h` (per-channel state), and the health
assessment in `src/runtime/runtime_health.cpp`.

- **Happy path unchanged.** When the sidecar persist succeeds, the order stays
  `Upsert` then `ApplyDuty`, preserving the crash-recovery record
  (`docs/WRITE_ORCHESTRATION.md` step 6).
- **Persist failure must not skip the write.** When `pending_store.Upsert`
  throws, the controller must still call `fan_writer.ApplyDuty` for the computed
  setpoint rather than returning early. The subsequent `ApplyDuty` result is
  handled by the existing success/failure paths
  (`src/control/channel_write.cpp:337-347`).
- **Sensor-safe command always actuates.** When `evaluation.safety_override` is
  set, the write must reach the actuator irrespective of sidecar persist success.
- **Observable backstop, not a silent skip.** A persist failure followed by a
  successful actuation must (a) keep emitting the
  `control_loop.sidecar_upsert_failed` event, (b) increment an additive
  per-channel counter for the condition, and (c) degrade the runtime health state
  so `--health` reflects the degraded condition. It must **not** increment
  `consecutive_write_failures` or open the write-failure breaker (the actuation
  succeeded), and must not, by this condition alone, drive a watchdog recycle.
- **Event logging is best-effort (REQ-WRITESAFE-06).** The additive counter is
  incremented before the `control_loop.sidecar_upsert_failed` emit, and the emit
  is wrapped so a throw from event serialization/append (e.g. a non-UTF-8
  exception message making the JSON dump throw, or an allocation failure) cannot
  veto the fan write.
- **Crash recovery preserved.** Because the reconcile/restore paths replay the
  captured baseline (`baseline_duty_raw`/`baseline_mode_raw`), which is stable
  across ticks, a stale-but-present sidecar entry still restores correctly; the
  in-memory `entries_` update means the next successful tick re-persists the file
  once the fault clears. **Accepted residual:** a first-write-with-failed-persist
  crash leaves no entry for that channel; the in-window commanded duty is a
  cooling command and the next worker re-establishes control (recorded, not
  mitigated, in v1).

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-WRITESAFE-01 | A failure to persist the pending-write sidecar entry must not prevent the computed fan duty from being applied: on a sidecar persist failure the control path must still call `ApplyDuty` for the computed setpoint. |
| REQ-WRITESAFE-02 | When `evaluation.safety_override` is set, the fan write must reach the actuator regardless of whether the sidecar persist succeeded. |
| REQ-WRITESAFE-03 | A sidecar persist failure followed by a successful actuation must be observable — emit `control_loop.sidecar_upsert_failed`, increment an additive per-channel counter, and degrade the runtime health state — and must not increment `consecutive_write_failures`, open the write-failure breaker, or by itself cause a watchdog recycle. |
| REQ-WRITESAFE-04 | Crash recovery must remain correct when a sidecar entry is stale or absent because a persist failed: reconcile/restore must restore the captured baseline. The first-write-with-failed-persist case (no entry for that channel) is an accepted residual recorded in §5. |
| REQ-WRITESAFE-05 | The change must be confined to the failure path: the computed duty, cadence, channel set, and control-computation identity are unchanged, and any new status/health field is additive to `docs/RUNTIME_HOME.md`. |
| REQ-WRITESAFE-06 | Event logging on the sidecar-persist-failure path is best-effort and must not veto the fan write: a throw from event serialization or append is swallowed, and `ApplyDuty` still runs for the computed setpoint. |

## 7. Data / schema deltas

- New/changed fields: an additive per-channel counter for sidecar persist
  failures (e.g. `consecutive_sidecar_persist_failures` or
  `sidecar_persist_failures`, `uint`, default `0`, optional in status output);
  reuse of the existing health-state vocabulary (`degraded`) for the new
  condition. No new event type is required (`control_loop.sidecar_upsert_failed`
  already exists).
- Config impact (`config/control.*.json`, `config/machines/*.json`): none.
- Schema/version impact: additive only; update `docs/RUNTIME_HOME.md` (status
  field + the health-degradation trigger) and `docs/WRITE_ORCHESTRATION.md`
  (failure-path behavior) at implementation. No existing runtime-home file,
  archive, or config becomes invalid.

## 8. CLI / config / operator surface deltas

- `--health` / `--status` reflect the degraded health state and the additive
  per-channel counter (read-only). No new operator write action.
- No new CLI subcommand or flag. UI is out of scope (`docs/MEASUREMENT_GATE.md`).
- Update `README.md` only if the `--health`/`--status` field list it documents
  changes; otherwise the doc updates are `docs/RUNTIME_HOME.md` and
  `docs/WRITE_ORCHESTRATION.md` per `AGENTS.md` §Change Checklist.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/write-actuation-sidecar-fault-decision-2026-06-17.md`](../write-actuation-sidecar-fault-decision-2026-06-17.md) | Durability-vs-availability direction (actuate-anyway, Option A); backstop = degraded health + event + counter, not breaker/recycle; the accepted first-write residual; confinement to the failure path. | Current (Accepted 2026-06-17) |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-WRITESAFE-01 | T | `.\scripts\Test-LocalCI.ps1` C++ test: a throwing pending-store with `src/hardware/simulated_fan_writer.cpp` asserts `ApplyDuty` still fires for the computed setpoint after a persist failure. |
| REQ-WRITESAFE-02 | T | C++ test: `safety_override` set + throwing pending-store asserts the actuator receives the 100% command. |
| REQ-WRITESAFE-03 | T, R | C++ test asserts the per-channel counter increments, health degrades, the `control_loop.sidecar_upsert_failed` event is emitted, and the breaker does not open / `consecutive_write_failures` stays `0`; review vs `docs/RUNTIME_HOME.md` (additive field + health trigger). |
| REQ-WRITESAFE-04 | T, R | C++ test: reconcile/restore with a stale and an absent entry restores the captured baseline; review vs `docs/WRITE_ORCHESTRATION.md` Reconciliation. |
| REQ-WRITESAFE-05 | R | Review vs `docs/CONTROL_PIPELINE_MATH.md` and `docs/MEASUREMENT_GATE.md`: computed duty/cadence/channels/identity unchanged; status field additive. |
| REQ-WRITESAFE-06 | T, R | C++ test: a persist failure whose event serialization throws (non-UTF-8 exception message) still reaches `ApplyDuty`; review that the pre-actuation event append in `channel_write.cpp` is wrapped best-effort. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Exact name of the additive per-channel counter and whether it is monotonic or "consecutive" (resets on a successful persist). | implementation | A "consecutive" counter that resets on a successful persist, mirroring `consecutive_write_failures` naming. |
| Whether the degraded-health condition clears automatically on the next successful persist or holds for a debounce window. | implementation | Clear on the next successful persist (the store self-heals), with the event recording onset. |

## 12. Measurement gate & dependencies

- **Measurement gate:** not crossed. The change is failure-path only; it does not
  change cadence, live channels, or mixed-input strategy, and adds no term to the
  control identity, so no characterization evidence is required before
  implementation (`docs/MEASUREMENT_GATE.md`).
- **Depends on:** the existing channel-write path (`src/control/channel_write.cpp`),
  the pending-writes store (`src/runtime/pending_writes.cpp`), and the health
  assessment (`src/runtime/runtime_health.cpp`). Cross-references but does not
  depend on the open-breaker-blocks-rising-demand finding
  (`src/control/channel_write.cpp:300-309`) and the restore/reconcile
  blocked-channel-guard finding (`third_party/SVG-MB-SIO/src/svg_mb_sio.cpp`
  restore), which are separate later rounds.
- **Build/test impact:** new C++ tests under `tests/cpp/` driven by
  `src/hardware/simulated_fan_writer.cpp` and an injectable throwing
  pending-store; doc updates to `docs/RUNTIME_HOME.md` and
  `docs/WRITE_ORCHESTRATION.md` per `AGENTS.md` §Change Checklist. No
  `docs/CONTROL_PIPELINE_MATH.md` change.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2 — runtime-reproduced H1, `review/repro/h1_repro_evidence.json`).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [x] 3. Required design decision record(s) written and marked current (§9 — `docs/write-actuation-sidecar-fault-decision-2026-06-17.md`, Current).
- [x] 4. Concrete `REQ-WRITESAFE-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — `Test-LocalCI`, contract review (§10), and mirrored in `docs/TRACEABILITY.md`.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline (failure-path only; computed duty unchanged; additive schema).
- [x] 7. Doctrine check: current behavior claims grounded with file:line; proposed behavior labeled as proposed; `must`/`should`/`is` used per `CLAUDE.md`; no undefined terms or unqualified vague adjectives.

> Accepted 2026-06-17: the maintainer authorized implementation of this feature
> this session (H1-first round). Accepted is build-authorized here; the feature
> becomes `Implemented` when §14 is filled and `docs/TRACEABILITY.md` results are
> updated in the same change.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-WRITESAFE-01 | pass | `tests/cpp/channel_write_tests.cpp::TestSidecarPersistFailureStillActuates` — CTest green (`Test-LocalCI` 13/13) | 2026-06-17 |
| REQ-WRITESAFE-02 | pass | `channel_write_tests.cpp::TestSafetyOverrideActuatesDespiteSidecarPersistFailure` (100% safe-mode command actuates past an open breaker) — CTest green | 2026-06-17 |
| REQ-WRITESAFE-03 | pass | `channel_write_tests.cpp::TestSidecarPersistFailureIncrementsCounterNotBreaker` + `...CounterResetsOnSuccess` + `...DegradesHealth` (`DegradedChannelCount`) + `...EmitsEvent` (event logged) — CTest green | 2026-06-17 |
| REQ-WRITESAFE-04 | pass | `channel_write_tests.cpp::TestSidecarBaselineSurvivesStaleAndAbsentEntry` (stale/absent sidecar baseline round-trip); reconcile→restore integration-covered (`tests/test_write_once.py`) — CTest green | 2026-06-17 |
| REQ-WRITESAFE-05 | pass | Review (R): change confined to the `channel_write.cpp` failure path; computed duty/cadence/channels/control identity unchanged; `consecutive_sidecar_persist_failures` is an additive status field | 2026-06-17 |
| REQ-WRITESAFE-06 | pass | `channel_write_tests.cpp::TestEventLogThrowDoesNotVetoActuation` — a non-UTF-8 exception message makes the failure-path event JSON dump throw; the wrapped best-effort emit swallows it and actuation still fires — CTest green | 2026-06-17 |

**Spec vs. implementation deltas:** Implemented per spec. Added a
`PendingWritesStoreInterface` seam (behavior-preserving testability refactor) so a
throwing pending-store can be injected; `PendingWritesStore` implements it and the
control write path (`TryApplyChannelSetpoint`) takes the interface. The counter is
named `consecutive_sidecar_persist_failures` (the §11 "consecutive, resets on a
successful persist" default); health degrades via `DegradedChannelCount` and the
counter is additive in `control_runtime.json`. **v0.3 hardening
(REQ-WRITESAFE-06):** the pre-actuation `control_loop.sidecar_upsert_failed` emit is
wrapped best-effort and the counter is incremented before it, so an event-logging
throw can no longer re-veto actuation — closing the residual the 2026-06-17 external
review flagged.
