# FEAT-0014: Reconcile and restore honor the blocked-channel guard

**Project:** svg-mb-control
**Status:** Draft (held — investigated; gap is real but not reachable by the shipped single-profile config, pending maintainer direction)   **Version:** 0.1   **Updated:** 2026-06-17
**Namespace:** `REQ-RESTOREGUARD-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/WRITE_ORCHESTRATION.md`
**Purpose:** propose that the startup reconcile and the in-process restore consult
the runtime write policy before replaying a stored baseline, so a restore cannot
write to a channel the control loop is forbidden to write — closing the bypass
cross-referenced from FEAT-0010 §12.

## 1. Summary

Normal control writes are gated by the runtime write policy: the control loop
calls `RuntimeFanAllowsWrite` before every duty write
(`src/control/channel_write.cpp:297`), which reads `effective_write_allowed =
write_allowed && !policy_blocked` (`src/platform/direct_runtime_snapshot.cpp:138-145`),
and the SIO writer's `set_fan_duty` independently refuses a blocked channel
(`third_party/SVG-MB-SIO/src/svg_mb_sio.cpp:221-223`). The shipped live policy
blocks channel 6 (`config/runtime_policy_write_live.json` `blocked_channels: [6]`;
Python test `test_shipped_live_runtime_policy_blocks_channel_6`,
`tests/test_config_contracts.py:465-468`).

The restore/reconcile path does not apply that guard. `ReconcilePendingWrites`
runs at every worker start (`src/app/app_main.cpp:298`) and, for each stored
sidecar entry, calls `writer->RestoreSavedState(entry.channel, ...)` directly
(`src/runtime/write_orchestrator.cpp:341-344`) with no policy consultation, even
though it already receives the resolved `RuntimeWritePolicy`. The SIO writer's
`RestoreSavedState` and the underlying `MbSioController::restore_saved_state`
also omit the check that `set_fan_duty` performs
(`third_party/SVG-MB-SIO/src/svg_mb_sio.cpp:259-277`, no `channel_blocked`).
A stored baseline for a policy-blocked channel is therefore replayed to hardware
on startup, bypassing the guard the control loop enforces. This spec proposes
(does not decide) that reconcile/restore consult the policy and skip-and-log a
blocked channel.

## 2. Problem & motivation  *(promotion gate 1)*

This is a named code/contract gap, statically verified against source on
2026-06-17 (review finding "Restore/reconcile paths bypass the blocked-channel /
writes-enabled guard", `review/svg-mb-control-review-20260617-team-review.md`,
cross-referenced from FEAT-0010 §12). It is **partial**: the missing guard is a
real defect on the restore/reconcile code path, but it is **not reachable by the
shipped single-profile configuration alone**, and where reachable its fail
direction is bounded and one-shot.

1. **The guard is enforced on the write path and absent on the restore path.**
   `set_fan_duty` checks `writes_enabled` then `channel_blocked` and returns
   `not_supported` for a blocked channel
   (`third_party/SVG-MB-SIO/src/svg_mb_sio.cpp:218-223`). `restore_saved_state`,
   `restore_fan_auto`, and `restore_all_fans` perform the open/availability/
   generation checks but never call `channel_blocked` or read `writes_enabled`
   (`:229-277`). The Control-layer wrappers mirror this: the SIO writer's
   `RestoreSavedState` calls straight through
   (`src/hardware/sio_fan_writer.cpp:282-298`) and the simulated writer's
   `RestoreSavedState` ignores `policy_` entirely
   (`src/hardware/simulated_fan_writer.cpp:249-277`), while both writers' `ApplyDuty`
   do consult the policy (`simulated_fan_writer.cpp:231-235`).
2. **`ReconcilePendingWrites` does not pre-check the policy.** It receives
   `const RuntimeWritePolicy& runtime_policy` and uses it only to construct the
   writer (`src/runtime/write_orchestrator.cpp:314`). The restore loop
   (`:341-361`) calls `RestoreSavedState` for every entry read from the sidecar
   without calling `RuntimeWritePolicyBlocksChannel`. By contrast the
   `--write-once` path in the same file checks `effective_write_allowed` before
   acting and returns exit `5` for a blocked channel
   (`src/runtime/write_orchestrator.cpp:151-165`), so the orchestrator already
   knows how to apply the guard — the reconcile path simply omits it.
3. **Not reachable by the shipped single-profile config.** With the live policy
   (`blocked_channels: [6]`), the control loop never writes a sidecar entry for
   channel 6: `RuntimeFanAllowsWrite` returns `false` at
   `src/control/channel_write.cpp:297`, which is upstream of the
   `pending_store.Upsert` at line 320, so no blocked-channel entry is ever
   persisted by the loop, and the loop controls channels 0-5 only. A
   blocked-channel sidecar entry can exist only from a prior profile that
   enumerated and did not block the channel, a profile swap, the calibration
   tool, or a hand-edited `pending_writes.json`. This precondition is why the
   verdict is partial, not a live defect.
4. **Bounded, one-shot fail direction when reached.** Reconcile would drive
   channel 6 (the AIO pump on the live board) to the stored `baseline_duty_raw`
   plus a sanitized mode; a low stale baseline under-drives the pump until the
   next external owner re-asserts. Two existing factors bound the harm: the SIO
   restore sanitizes `mode_raw == 0` to `0x40` (NCT6701 SmartFan auto)
   (`third_party/SVG-MB-SIO/src/fan_sio.cpp:923-928`), a safe direction; and the
   pump is independently owned by FanControl (MEMORY: control-startup incident
   2026-06-14). The action is a single startup replay, not a sustained loop.

The guard exists precisely to keep the controller's writes off a policy-blocked
channel; restore/reconcile ignore it. A fix must apply the same
`writes_enabled` + `blocked_channels` decision to the restore replay without
changing the unblocked-channel crash-recovery behavior FEAT-0010 depends on.

## 3. Goals & non-goals

**Goals**
- A startup reconcile must not replay a stored baseline to a channel the runtime
  write policy blocks; a blocked channel is skipped and the skip is logged.
- The in-process restore (the `--write-once` hold-then-restore and any future
  caller) must not write a baseline to a policy-blocked channel.
- A skipped-because-blocked reconcile entry is observable (event) and does not
  count as a restore failure that fails the reconcile exit code.

**Non-goals**
- Does not change restore behavior for an unblocked channel; the FEAT-0010
  crash-recovery replay of channels 0-5 is unchanged.
- Does not change the computed duty, cadence, channel set, curve, or input
  strategy.
- Does not decide whether a blocked channel's stale sidecar entry is *cleared*
  on skip or *retained* (an open decision, §11) — the held posture leaves that to
  the maintainer.
- Does not change the watchdog restart policy or the startup-fatal-on-read-failure
  behavior (a separate finding, §12).

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| No fan write / authority change outside an explicit live task | `AGENTS.md` §Live Runtime Safety | The change only *removes* a write (skips a blocked-channel restore); it adds no new write site and no new live action. It makes the restore path obey the same policy the live write path already obeys. |
| Shipped 250 ms cadence / channel set / input strategy is the measured baseline | `docs/MEASUREMENT_GATE.md` | Reconcile/restore is a startup/one-shot path, not the steady loop; cadence, live channels (0-5), and input strategy are unchanged, so the gate baseline does not move. |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | No control-law term is touched; restore replays a stored raw byte, not a computed duty. |
| Runtime sidecar / status / manifest schema stays backward-compatible | `docs/RUNTIME_HOME.md` | No schema change is required to skip a blocked channel; a new `reconcile.*` event reuses the existing event vocabulary. Any retain-vs-clear choice (§11) stays within the existing sidecar schema. |
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | The change is confined to in-repo control/runtime code and the vendored `third_party/SVG-MB-SIO` restore functions; no external dependency. |

## 5. Behavior specification

Behavior is **proposed** (not yet implemented, not yet decided). It would live in
`src/runtime/write_orchestrator.cpp` (`ReconcilePendingWrites` and the
`RunWriteOnce` restore), and optionally the vendored restore functions in
`third_party/SVG-MB-SIO/src/svg_mb_sio.cpp` (`restore_saved_state`,
`restore_fan_auto`, `restore_all_fans`).

- **Reconcile skips a blocked channel.** Before calling `RestoreSavedState` for a
  sidecar entry, `ReconcilePendingWrites` would consult
  `RuntimeWritePolicyBlocksChannel(runtime_policy, entry.channel)` (and
  `runtime_policy.writes_enabled`). When the channel is blocked or writes are
  disabled, it would not call `RestoreSavedState`; it would emit a distinct event
  (proposed `reconcile.restore_skipped_blocked`) and continue to the next entry.
- **A skip is not a restore failure.** A blocked-channel skip would not set
  `any_failure` and would not change the reconcile exit code on its own (it is an
  intended refusal, not a restore error).
- **In-process restore honors the policy.** The `--write-once` restore at
  `src/runtime/write_orchestrator.cpp:250-261` already cannot reach a blocked
  channel today (it returns exit `5` before writing, `:151-165`); the proposal
  keeps that guarantee and extends the same refusal to any future restore caller.
- **Optional defense in depth at the writer boundary.** The vendored restore
  functions could mirror `set_fan_duty`'s `channel_blocked` check so the guard is
  enforced at the same layer for every restore caller, not only the reconcile
  loop. Whether to change vendored code or guard only at the Control layer is an
  open decision (§11).
- **Unblocked channels unchanged.** For channels 0-5 under the live policy, the
  reconcile/restore replay is byte-for-byte the current behavior, preserving the
  FEAT-0010 crash-recovery guarantee.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-RESTOREGUARD-01 | `ReconcilePendingWrites` must not call `RestoreSavedState` for a sidecar entry whose channel is blocked by the runtime write policy (`RuntimeWritePolicyBlocksChannel`) or when `writes_enabled` is false; such an entry is skipped without writing to hardware. |
| REQ-RESTOREGUARD-02 | A blocked-channel skip must be observable as a distinct reconcile event (proposed `reconcile.restore_skipped_blocked`) and must not be counted as a restore failure for the reconcile exit code. |
| REQ-RESTOREGUARD-03 | The in-process restore path (`RunWriteOnce` hold-then-restore, and any future restore caller) must not write a baseline to a channel the runtime write policy blocks; the existing `--write-once` pre-write refusal (`write_orchestrator.cpp:151-165`) is preserved. |
| REQ-RESTOREGUARD-04 | Restore/reconcile behavior for an unblocked channel is unchanged: the captured-baseline replay for channels the policy allows (the FEAT-0010 crash-recovery path) produces the same hardware writes as before. |
| REQ-RESTOREGUARD-05 | The change is confined to the restore/reconcile path: the computed duty, cadence, live channel set (0-5), and control-computation identity are unchanged, and any new event reuses the existing `reconcile.*` event vocabulary with no `docs/RUNTIME_HOME.md` schema break. |

## 7. Data / schema deltas

- New/changed fields: none required. The skip is expressed as a new
  `reconcile.*` event string (proposed `reconcile.restore_skipped_blocked`),
  which reuses the existing event-log shape; no new status field and no sidecar
  schema change.
- Config impact (`config/control.*.json`, `config/runtime_policy_*.json`): none.
  The feature reads the existing `blocked_channels` / `writes_enabled` policy; it
  adds no config key.
- Schema/version impact: additive event only; doc updates at implementation would
  be `docs/WRITE_ORCHESTRATION.md` (Reconciliation behavior) and, if a retain-vs-
  clear choice changes sidecar lifecycle, a note in `docs/RUNTIME_HOME.md`. No
  existing runtime-home file, archive, or config becomes invalid.

## 8. CLI / config / operator surface deltas

- No new CLI subcommand or flag. The reconcile/`--write-once` surfaces are
  unchanged; only their internal policy enforcement changes.
- The new `reconcile.restore_skipped_blocked` event appears in the runtime event
  log (read-only operator visibility). No new operator write action.
- UI is out of scope (`docs/MEASUREMENT_GATE.md`).

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

For this held-Draft intake no separate decision file is created; the held posture
means nothing here authorizes code. The direction is recorded as proposed,
pending maintainer selection.

| Decision doc | Decision it must settle | Status |
|---|---|---|
| (none yet — to be written as `docs/reconcile-restore-blocked-channel-guard-decision-YYYY-MM-DD.md` if promoted) | Where the guard lives (Control-layer pre-check in `ReconcilePendingWrites`/`RunWriteOnce` only, vs. also mirroring `channel_blocked` into the vendored restore functions); whether a skipped blocked-channel sidecar entry is cleared or retained; whether `--write-once`'s existing exit-5 refusal is sufficient for the in-process restore or needs an explicit restore-time check. | Proposed (pending maintainer direction) |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-RESTOREGUARD-01 | T, R | C++/integration test: a sidecar entry for a blocked channel plus a policy with `blocked_channels` containing it asserts `RestoreSavedState` is not invoked for that channel (e.g. an injected/simulated writer records calls); review vs `third_party/SVG-MB-SIO/src/svg_mb_sio.cpp:218-223` guard parity. |
| REQ-RESTOREGUARD-02 | T, R | Test asserts the `reconcile.restore_skipped_blocked` event is emitted and the reconcile exit code is `0` when the only non-restored entry was a policy skip; review vs `docs/WRITE_ORCHESTRATION.md` Reconciliation. |
| REQ-RESTOREGUARD-03 | T, R | Test of the `RunWriteOnce`/restore path with a blocked channel asserts no baseline write reaches the actuator; review that the existing `write_orchestrator.cpp:151-165` exit-5 refusal is preserved. |
| REQ-RESTOREGUARD-04 | T | Test: an unblocked-channel sidecar entry is still restored (same `RestoreSavedState` call and arguments as today), guarding the FEAT-0010 crash-recovery path against regression. |
| REQ-RESTOREGUARD-05 | R | Review vs `docs/CONTROL_PIPELINE_MATH.md` and `docs/MEASUREMENT_GATE.md`: computed duty/cadence/live-channel set/identity unchanged; event additive; no schema break. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Guard placement: Control-layer pre-check only (`ReconcilePendingWrites`/`RunWriteOnce`) vs. also mirroring `channel_blocked` into the vendored `restore_saved_state`/`restore_fan_auto`/`restore_all_fans`. | implementation | Control-layer pre-check first (no vendored-code change), because the reconcile loop already holds the resolved `RuntimeWritePolicy`; add the vendored guard only if a second restore caller appears. |
| Whether a skipped blocked-channel sidecar entry is cleared (so it does not re-trigger every start) or retained (so a later profile that unblocks the channel can restore it). | implementation | Retain the entry and skip it each start, because clearing it would silently drop a baseline a later profile swap might need; revisit if retained skips become log noise. |
| Whether the existing `--write-once` exit-5 refusal suffices for the in-process restore or an explicit restore-time policy check is added. | implementation | Add an explicit restore-time check so the guarantee does not depend on the pre-write path's ordering. |

## 12. Measurement gate & dependencies

- **Measurement gate:** not crossed. The change affects a startup/one-shot
  reconcile and the `--write-once` restore, not the steady control loop; it does
  not change cadence, live channels (0-5), or mixed-input strategy, and adds no
  term to the control identity, so no characterization evidence is required
  before implementation (`docs/MEASUREMENT_GATE.md`).
- **Depends on / cross-references:** the runtime write policy
  (`src/runtime/runtime_write_policy.*`, `RuntimeWritePolicyBlocksChannel`), the
  reconcile orchestrator (`src/runtime/write_orchestrator.cpp:291-375`), and the
  vendored restore functions
  (`third_party/SVG-MB-SIO/src/svg_mb_sio.cpp:259-277`). This is the
  restore/reconcile blocked-channel-guard finding cross-referenced from
  **FEAT-0010 §12**; FEAT-0010 (sidecar-persistence fault) is the neighboring,
  separately-scoped write-path round and is not a build dependency. The
  corrupt-sidecar startup-fatal finding (review finding 2,
  `src/runtime/write_orchestrator.cpp:296-307`) is a distinct hazard and out of
  scope here.
- **Build/test impact:** new tests under `tests/` (C++ smoke or pytest) using a
  call-recording / simulated writer and a policy with `blocked_channels`; doc
  update to `docs/WRITE_ORCHESTRATION.md` per `AGENTS.md` §Change Checklist. No
  `docs/CONTROL_PIPELINE_MATH.md` change.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2 — statically verified against `svg_mb_sio.cpp:218-277`, `write_orchestrator.cpp:341-361`; corroborated by `review/svg-mb-control-review-20260617-team-review.md`).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [ ] 3. Required design decision record(s) written and marked current (§9 — not written; the held intake leaves guard-placement and retain-vs-clear to the maintainer. This is the held gate.).
- [x] 4. Concrete `REQ-RESTOREGUARD-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — `Test-LocalCI`, contract review (§10), and mirrored in `docs/TRACEABILITY.md`.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline (the change only removes a blocked-channel write; startup/one-shot path; live channels 0-5 unchanged).
- [x] 7. Doctrine check: current behavior claims grounded with file:line; proposed behavior labeled as proposed; `must`/`should`/`is` used per `CLAUDE.md`; no undefined terms or unqualified vague adjectives.

> Held at Draft 2026-06-17: the gap is real on the restore/reconcile code path but
> is not reachable by the shipped single-profile configuration (the control loop's
> pre-Upsert `RuntimeFanAllowsWrite` guard prevents a blocked-channel sidecar
> entry, and the fail direction is bounded, one-shot, and mode-sanitized).
> Promotion to Accepted is the maintainer's call and requires the §9 decision
> record (guard placement + retain-vs-clear). Nothing here authorizes code.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

Not started — the feature is held at Draft. Each row is filled after
implementation, which is not authorized until the §9 decision record is written
and the maintainer promotes the spec.

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-RESTOREGUARD-01 | | | |
| REQ-RESTOREGUARD-02 | | | |
| REQ-RESTOREGUARD-03 | | | |
| REQ-RESTOREGUARD-04 | | | |
| REQ-RESTOREGUARD-05 | | | |

**Spec vs. implementation deltas:** none yet (not implemented).
