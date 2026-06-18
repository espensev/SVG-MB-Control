# FEAT-0019: Sidecar persistence off the actuation hot path

**Project:** svg-mb-control
**Status:** Draft   **Version:** 0.1   **Updated:** 2026-06-18
**Namespace:** `REQ-WRITEHOT-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/WRITE_ORCHESTRATION.md`, `docs/RUNTIME_HOME.md`,
`docs/features/FEAT-0010-write-actuation-sidecar-fault.md`
**Purpose:** remove the synchronous `pending_writes.json` atomic file-replace from
the actuation critical path by persisting synchronously only on a recovery-relevant
identity change and deferring same-baseline setpoint churn to the existing
once-per-tick end-of-tick `Flush()`, so no fsync'd file-replace runs before
`ApplyDuty` during a ramp — with no change to crash-recovery behavior.

> Draft / design capture. This is not implementation authorization. The direction
> is recorded in `docs/control-latency-reduction-design-2026-06-18.md`
> (D-WRITEHOT-1, Proposed). This is the build-ready direction of the latency set:
> behavior-preserving for recovery, code-local, and not gate-crossing.

## 1. Summary

Every changed control write persists the pending-write sidecar synchronously
before touching hardware: `TryApplyChannelSetpoint` calls
`pending_store.Upsert(entry)` (`src/control/channel_write.cpp`), which calls
`Persist()` → `WritePendingWrites` → `WriteJsonFileAtomic`
(`src/runtime/pending_writes.cpp`, `src/runtime/json_io.cpp`) — a temp write,
`FlushFileBuffers`, and `MoveFileEx` with `WRITE_THROUGH` — and only then runs
`fan_writer.ApplyDuty`. During a ramp the setpoint changes nearly every tick, so an
fsync'd file replace sits on the critical path ahead of every hardware write; under
load with disk contention this is the documented Layer-0 stall surface. The sidecar
exists to let crash recovery restore a channel's captured baseline; recovery
(`ReconcilePendingWrites`) reads only `channel`, `baseline_duty_raw`, and
`baseline_mode_raw`, and ignores the churning `target_pct`. This feature persists
synchronously only when that recovery-relevant identity changes and defers
same-baseline churn to the end-of-tick `Flush()` (off the hot path), so no fsync'd
file-replace runs before `ApplyDuty` during a ramp and a tick's per-channel churn
batches into one write, while keeping the crash-recovery record correct.

## 2. Problem & motivation  *(promotion gate 1)*

Named code/contract gap, verified against source:

1. **A synchronous atomic file replace is on the critical path per changed write.**
   `PendingWritesStore::Upsert` ends with `Persist()`
   (`src/runtime/pending_writes.cpp`), comment: "Upsert MUST persist
   synchronously." `Persist` → `WritePendingWrites` → `WriteJsonFileAtomic`
   (`src/runtime/json_io.cpp`) writes a temp file, `FlushFileBuffers`, then
   `MoveFileExW(... MOVEFILE_WRITE_THROUGH)` with retry/backoff.
   `TryApplyChannelSetpoint` calls `Upsert` immediately before
   `fan_writer.ApplyDuty` (`src/control/channel_write.cpp`).
2. **During a ramp this is once per tick.** With the rate limiter producing small
   intermediate steps each tick (`docs/control-latency-reduction-design-2026-06-18.md`
   §2), the setpoint changes nearly every tick, so a ramp issues one fsync'd file
   replace per `~250 ms`, each before the hardware write.
3. **Recovery does not need the churning field.** `ReconcilePendingWrites`
   (`src/runtime/write_orchestrator.cpp`) restores via
   `writer->RestoreSavedState(entry.channel, entry.baseline_duty_raw,
   entry.baseline_mode_raw, ...)` — it reads only those three fields and never reads
   `target_pct`, `requested_hold_ms`, or `started_iso`. Health
   (`src/runtime/runtime_health.cpp`) reads only the sidecar's *readability*
   (`pending_writes_unreadable`), not its contents. With `control_hold_ms = 0`
   (`release/control.json`) the baseline is captured once and held for the session,
   so the recovery-relevant identity `(channel, baseline_duty_raw, baseline_mode_raw)`
   is stable across an entire ramp — yet the sidecar is rewritten every tick anyway.

This is the general fix for the Layer-0 sidecar-race stall surface
(`docs/cpu-loop-stall-reproduction-findings-2026-06-16.md`): FEAT-0010 made a
persist *fault* non-vetoing, but the persist itself still runs synchronously on the
hot path. This feature removes the synchronous pre-`ApplyDuty` persist for
same-baseline churn, deferring it to the existing end-of-tick `Flush()`.

## 3. Goals & non-goals

**Goals**
- Persist the pending-write sidecar synchronously only when a channel's
  recovery-relevant identity `(channel, baseline_duty_raw, baseline_mode_raw)` changes
  or its entry is first created; defer same-baseline churn to the existing end-of-tick
  `Flush()`, so no synchronous persist runs before `ApplyDuty` during a ramp.
- Preserve the crash-recovery invariant exactly: every active channel's captured
  baseline is recorded in the sidecar before its first `ApplyDuty`, so recovery can
  still restore it.
- Keep the change code-local to the store and its call site; no schema change, no
  new gate crossing.

**Non-goals**
- No change to *what* recovery restores (`RestoreSavedState` from the baseline) or
  to the FEAT-0010 non-vetoing persist-fault behavior.
- No change to the queued-removal + end-of-tick `Flush()` path
  (`src/control/tick_runner.cpp`), which is already off the synchronous write path.
- No reordering of persist relative to `ApplyDuty` (record-intent-before-actuation
  ordering is kept for the activation write).
- No change to cadence, curves, channels, control identity, or write policy.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| A crash mid-write must leave a sidecar record from which recovery restores the captured baseline | `src/runtime/pending_writes.cpp` (Upsert "MUST persist synchronously"), `docs/WRITE_ORCHESTRATION.md` | The activation write (first entry for a channel, or a baseline change) still persists synchronously before `ApplyDuty`. Subsequent same-baseline `target_pct` churn is deferred to the end-of-tick `Flush()`, and recovery never reads `target_pct`, so the recoverable record is unchanged. |
| Runtime sidecar / status schema stays backward-compatible | `docs/RUNTIME_HOME.md` | No schema change. `pending_writes.json` fields are unchanged; only the *position* of the `target_pct`/`started_iso` write moves (to the end-of-tick `Flush()`), leaving them at most one tick stale, and their documented meaning is clarified accordingly. No consumer reads them as authoritative per-tick current (verified: `runtime_health.cpp`, `write_orchestrator.cpp`). |
| Write actuation survives a sidecar-persistence fault | `docs/features/FEAT-0010-write-actuation-sidecar-fault.md` (`REQ-WRITESAFE-*`) | The FEAT-0010 non-vetoing path is unchanged **for actuation**: a persist that runs and throws still falls through to `ApplyDuty`. The persist-failure health *signal* does change, and must be corrected: a deferred same-baseline `Upsert` returns normally and would otherwise clear `consecutive_sidecar_persist_failures` while a failed record is still missing — REQ-WRITEHOT-06 requires the counter to clear only on an actual successful persist (a cross-feature change at the `channel_write.cpp` reset site, §11). |
| No fan write / authority change outside an explicit live task | `AGENTS.md` §Live Runtime Safety | No new write site; the same `ApplyDuty` call, fewer sidecar writes ahead of it. |
| Repo stays standalone; cadence/channels are the measured baseline | `AGENTS.md` §Repo Boundary, `docs/MEASUREMENT_GATE.md` | Code-local change in `src/runtime/pending_writes.cpp` and `src/control/channel_write.cpp`; cadence, channels, and input strategy are untouched, so the gate baseline does not move. |

## 5. Behavior specification

The change lives in `PendingWritesStore` (`src/runtime/pending_writes.cpp`) and is
transparent to `TryApplyChannelSetpoint` (`src/control/channel_write.cpp`), which
keeps calling `Upsert` once per changed write.

- **Identity-gated synchronous persist.** `Upsert(entry)` updates the in-memory
  entry as today. It calls `Persist()` (the synchronous atomic write) only when, for
  that channel, no entry existed before (first activation) **or** the stored
  `baseline_duty_raw` / `baseline_mode_raw` differ from the incoming entry (baseline
  re-capture). When an entry already exists with the same baseline and only
  `target_pct` / `requested_hold_ms` / `started_iso` differ, the in-memory entry is
  updated and the store is marked dirty, but **no synchronous file write runs before
  `ApplyDuty`** — the deferred write happens at the end-of-tick `Flush()`. So during a
  ramp no fsync'd file-replace sits on the hot path, and a tick's per-channel churn
  batches into one end-of-tick write.
- **Activation write preserved.** The first write that makes a channel active
  persists synchronously before `ApplyDuty`, so the record-intent-before-actuation
  ordering and the crash-recovery guarantee hold for every active channel.
- **On-disk `target_pct` / `started_iso` are at most one tick stale.** The deferred
  same-baseline write is flushed at end of tick (`src/control/tick_runner.cpp`), so the
  on-disk values trail the in-memory entry by at most one tick. They are advisory only:
  recovery and health both ignore `target_pct` (§2 item 3, §4), so no consumer depends
  on a per-tick-current value.
- **Persist-failure health stays accurate.** A deferred same-baseline `Upsert` returns
  normally without persisting, so the existing `channel_write.cpp` reset of
  `consecutive_sidecar_persist_failures` must not run on it; the counter clears only on
  an actual successful persist (REQ-WRITEHOT-06). The per-tick `Flush()` re-attempts a
  failed deferred persist, preserving the FEAT-0010 self-heal.
- **Removal/restore unchanged.** `QueueRemove` + `Flush` behavior is unchanged; a
  channel restored to baseline still has its entry removed via the batched path.
- **Fault behavior unchanged for actuation.** When a persist does run and throws, the
  FEAT-0010 path applies: the in-memory entry is already updated, the failure is
  recorded (`consecutive_sidecar_persist_failures`, best-effort event), and control
  falls through to `ApplyDuty`.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-WRITEHOT-01 | `PendingWritesStore::Upsert` must perform the synchronous `Persist()` only when the target channel has no existing entry, or when the incoming `baseline_duty_raw` / `baseline_mode_raw` differ from the stored entry; an `Upsert` that changes only `target_pct` / `requested_hold_ms` / `started_iso` for an existing same-baseline entry must not perform a synchronous file write before `ApplyDuty` (it marks the store dirty for the end-of-tick `Flush()` instead — see REQ-WRITEHOT-04). |
| REQ-WRITEHOT-02 | Crash recovery must be unchanged: a sidecar produced under the identity-gated persist must still let `ReconcilePendingWrites` restore each active channel via `(channel, baseline_duty_raw, baseline_mode_raw)`; no recovery-relevant field is dropped or stale at the time of the channel's first `ApplyDuty`. |
| REQ-WRITEHOT-03 | The first write that activates a channel (entry created, baseline captured) must persist synchronously before `fan_writer.ApplyDuty`, preserving record-intent-before-actuation ordering for every channel that actuates. |
| REQ-WRITEHOT-04 | A same-baseline-only update must mark the store dirty and be written by the existing end-of-tick `Flush()` rather than persisted synchronously before `ApplyDuty`; the queued-removal + `Flush()` path for restore must be unchanged; and a later baseline re-capture for a channel must trigger a fresh synchronous `Persist()`. |
| REQ-WRITEHOT-05 | The `pending_writes.json` schema must be unchanged and backward-compatible; the documented meaning of `target_pct` / `started_iso` is clarified in `docs/RUNTIME_HOME.md` as advisory and at most one tick stale (written at the end-of-tick `Flush()`, not synchronously per change), and no live consumer (status/health/recovery) may depend on a per-tick-current value. |
| REQ-WRITEHOT-06 | A deferred (skipped-synchronous) same-baseline `Upsert` must not clear FEAT-0010's `consecutive_sidecar_persist_failures` health counter; the counter must clear only on an actual successful persist, so a failed identity-change persist stays signalled (degraded health) until a persist succeeds. This requires changing the counter-reset site in `src/control/channel_write.cpp` — a cross-feature change with FEAT-0010 recorded in §11. |

## 7. Data / schema deltas

- New/changed fields: none. `pending_writes.json` keeps `channel`,
  `baseline_duty_raw`, `baseline_mode_raw`, `target_pct`, `requested_hold_ms`,
  `started_iso`, `child_pid` (`src/runtime/pending_writes.cpp`
  `PendingWriteEntryToJson`).
- Config impact: none.
- Schema/version impact: none. The sidecar `schema_version` stays `1`. The only
  observable change is that the `target_pct` / `started_iso` write for a same-baseline
  change moves to the end-of-tick `Flush()` rather than running synchronously before
  `ApplyDuty`, so on disk they are at most one tick stale; their documented meaning is
  clarified in `docs/RUNTIME_HOME.md`. Existing sidecars and recovery stay valid.

## 8. CLI / config / operator surface deltas

- No CLI, flag, or config change.
- Doc update at implementation: `docs/RUNTIME_HOME.md` (clarify the `target_pct` /
  `started_iso` "≤1 tick stale / advisory" semantics and the identity-gated
  synchronous persist) and `docs/WRITE_ORCHESTRATION.md` (the persist-position
  behavior), per `AGENTS.md` §Change Checklist. No `README.md` change.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/control-latency-reduction-design-2026-06-18.md`](../control-latency-reduction-design-2026-06-18.md) (D-WRITEHOT-1) | Gate the synchronous persist on the recovery-relevant identity `(channel, baseline_duty_raw, baseline_mode_raw)`; defer same-baseline `target_pct` churn to the batched end-of-tick `Flush()`; correct the FEAT-0010 counter-reset so a deferred `Upsert` does not falsely clear `consecutive_sidecar_persist_failures`; do not reorder persist vs `ApplyDuty` for the activation write; add no new field. | Current (accepted 2026-06-18) |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-WRITEHOT-01 | T | `.\scripts\Test-LocalCI.ps1` C++ test (`tests/cpp/`, persist detected by sidecar file existence over a temp runtime home): an `Upsert` changing only `target_pct` on a same-baseline entry performs no synchronous file write, while a first-entry and a baseline change each persist one synchronously. |
| REQ-WRITEHOT-02 | T, R | C++ test: a sidecar produced through several same-baseline `Upsert`s is reconciled by the reconcile path to restore the captured baseline for each channel; review vs `ReconcilePendingWrites` (`src/runtime/write_orchestrator.cpp`) that only `(channel, baseline_duty_raw, baseline_mode_raw)` is consumed. |
| REQ-WRITEHOT-03 | T | C++ test: the first `Upsert` for a channel persists before the simulated `ApplyDuty` (order asserted via the existing `simulated_fan_writer` + a persist hook), so an activation record exists on disk before actuation. |
| REQ-WRITEHOT-04 | T | C++ test: a same-baseline update performs no synchronous persist but is written by a following `Flush()`; the `QueueRemove` + `Flush` removal path is unchanged; a baseline re-capture triggers a fresh synchronous persist that carries the latest in-memory `target_pct`. |
| REQ-WRITEHOT-05 | R | Review `docs/RUNTIME_HOME.md` (clarified `target_pct`/`started_iso` ≤1-tick-stale/advisory semantics; schema unchanged) and that no consumer (`runtime_health.cpp`, `write_orchestrator.cpp`, status) reads `target_pct` as authoritative per-tick current. |
| REQ-WRITEHOT-06 | T, R | C++ test: after a forced identity-change persist failure, a following same-baseline `Upsert` does not reset `consecutive_sidecar_persist_failures` (counter clears only on a successful persist); review the changed reset site in `src/control/channel_write.cpp` vs FEAT-0010. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Whether to add a debug counter for skipped persists (observability) | implementation | omit; the existing `total_writes` and event log already mark actuation, and adding a field touches `RUNTIME_HOME.md` for little value. |
| How to correct the FEAT-0010 counter so a deferred same-baseline `Upsert` does not falsely clear `consecutive_sidecar_persist_failures` (REQ-WRITEHOT-06) | implementation | **Resolved 2026-06-18 (D-WRITEHOT-1):** two-point reset. `Upsert` returns a `bool persisted`; `channel_write.cpp` clears the counter only when true (kills the *false clear*). `Flush()` also returns a `bool`, and `tick_runner` clears the counter for all `context.channels` on a successful flush, because a full-file `Persist()` makes every channel's record current (kills the *stuck-degraded* case after a failed activation self-heals via the batched write). Both points are needed: an identity-change `Upsert` sets `dirty_=false`, so a Flush-only reset would never fire. |
| Whether to extend the same identity-gating to the free-function `UpsertPendingWrite` (write-once path) | implementation | leave `UpsertPendingWrite` (write-once orchestrator) as-is; it persists once per write-once invocation, not per tick, so it is not on a ramp hot path. |

## 12. Measurement gate & dependencies

- **Measurement gate:** not crossed. Cadence, channels, and mixed-input strategy
  are unchanged; this is a write-path I/O reduction, not a cadence change
  (`docs/MEASUREMENT_GATE.md`). It reduces, never increases, synchronous writes.
- **Depends on:** `PendingWritesStore` (`src/runtime/pending_writes.cpp`), the call
  site (`src/control/channel_write.cpp` `TryApplyChannelSetpoint`), and the existing
  end-of-tick `Flush()` (`src/control/tick_runner.cpp`). Builds on the FEAT-0010
  non-vetoing persist path. Independent of FEAT-0017 and FEAT-0018.
- **Build/test impact:** new C++ tests under `tests/cpp/` (synchronous-persist by
  change kind; recovery-equivalence; activation-ordering; deferred-then-flush;
  counter-not-cleared-on-deferred-`Upsert`). A small change to the FEAT-0010
  counter-reset site in `src/control/channel_write.cpp` (REQ-WRITEHOT-06). Doc updates
  to `docs/RUNTIME_HOME.md` and `docs/WRITE_ORCHESTRATION.md`. No
  `docs/CONTROL_PIPELINE_MATH.md` change.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2 — synchronous `Persist()` on the hot path per changed write; recovery verified to ignore `target_pct`).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [x] 3. Required design decision record(s) written and marked current (§9 — `docs/control-latency-reduction-design-2026-06-18.md` D-WRITEHOT-1 promoted to Current 2026-06-18, recording the REQ-WRITEHOT-06 two-point counter-reset mechanism).
- [x] 4. Concrete `REQ-WRITEHOT-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — C++ tests, contract review (§10), mirrored in `docs/TRACEABILITY.md`.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline (code-local; cadence/channels unchanged; strictly fewer synchronous writes).
- [x] 7. Doctrine check: behavior claims grounded with file paths; proposed behavior labeled as proposed; `must`/`should`/`is` per `CLAUDE.md`; no undefined terms.

> Held at Draft (gate 3 open): the direction is captured and verification is mapped,
> but the decision record is Proposed pending maintainer authorization. This is the
> most build-ready member of the latency set — promoting D-WRITEHOT-1 to Current and
> settling the REQ-WRITEHOT-06 counter-reset mechanism (§11) is the remaining work
> before implementation.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-WRITEHOT-01 | | | |
| REQ-WRITEHOT-02 | | | |
| REQ-WRITEHOT-03 | | | |
| REQ-WRITEHOT-04 | | | |
| REQ-WRITEHOT-05 | | | |
| REQ-WRITEHOT-06 | | | |

**Spec vs. implementation deltas:** <record at implementation.>
