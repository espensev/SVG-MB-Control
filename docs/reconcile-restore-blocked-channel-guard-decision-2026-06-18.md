# Decision: reconcile/restore honor the blocked-channel guard (Control-layer pre-check, retain the entry)

**Project:** svg-mb-control
**Status:** Current (Accepted 2026-06-18)
**Owning feature:** `docs/features/FEAT-0014-reconcile-restore-blocked-channel-guard.md`
(`REQ-RESTOREGUARD-*`)
**Companion to:** `AGENTS.md`, `docs/WRITE_ORCHESTRATION.md`,
`docs/RUNTIME_HOME.md`, `docs/MEASUREMENT_GATE.md`,
`docs/features/FEAT-0010-write-actuation-sidecar-fault.md`

## Context

Normal control writes are gated by the runtime write policy
(`RuntimeFanAllowsWrite`, `src/control/channel_write.cpp:297`) and the SIO writer's
`set_fan_duty` independently refuses a blocked channel
(`third_party/SVG-MB-SIO/src/svg_mb_sio.cpp:218-223`); the shipped live policy blocks
channel 6. The restore/reconcile path does **not** apply that guard:
`ReconcilePendingWrites` calls `writer->RestoreSavedState(entry.channel, ...)` for
every sidecar entry (`src/runtime/write_orchestrator.cpp:341-344`) with no policy
consultation, even though it already holds the resolved `RuntimeWritePolicy`, and
the vendored `restore_saved_state` omits the `channel_blocked` check
(`svg_mb_sio.cpp:259-277`). A stored baseline for a blocked channel is therefore
replayed to hardware on startup, bypassing the guard the loop enforces.

**Partial finding:** the gap is real on the code path but **not reachable by the
shipped single-profile config** — the loop never persists a blocked-channel entry
(the pre-`Upsert` `RuntimeFanAllowsWrite` guard, `channel_write.cpp:297`), so a
blocked-channel entry can exist only from a prior/other profile, the calibration
tool, or a hand-edited sidecar — and where reached the fail direction is bounded,
one-shot, and mode-sanitized. Team review 2026-06-17; cross-referenced from
FEAT-0010 §12.

## Options considered

- **Guard placement:** Control-layer pre-check in `ReconcilePendingWrites` /
  `RunWriteOnce` only, vs. also mirroring `channel_blocked` into the vendored
  `restore_saved_state` / `restore_fan_auto` / `restore_all_fans`.
- **Skipped entry lifecycle:** clear the blocked entry on skip vs. retain it.
- **In-process restore:** rely on the existing `--write-once` exit-5 refusal vs. add
  an explicit restore-time policy check.

## Decision

**Adopt: a Control-layer pre-check, retaining the skipped entry, with an explicit
restore-time check.** Reasons:

- **Control-layer pre-check first (no vendored change):** `ReconcilePendingWrites`
  already holds the resolved `RuntimeWritePolicy`, so consulting
  `RuntimeWritePolicyBlocksChannel` + `writes_enabled` before `RestoreSavedState` is
  the smallest correct change and keeps the guard in first-party code
  (`REQ-RESTOREGUARD-01`). Mirroring the check into vendored restore functions is
  deferred until a second restore caller appears (defense-in-depth, not v1).
- **Retain the skipped entry:** clearing it would silently drop a baseline a later
  profile that unblocks the channel might need; retaining and skipping each start is
  the conservative choice (revisit only if retained skips become log noise).
- **Explicit restore-time check:** add the policy check at the restore site so the
  in-process guarantee does not depend on the `--write-once` pre-write path's
  ordering (`REQ-RESTOREGUARD-03`); the existing exit-5 refusal
  (`write_orchestrator.cpp:151-165`) is preserved.
- **Observable, not a failure:** a blocked-channel skip emits
  `reconcile.restore_skipped_blocked` and does **not** set `any_failure` or change
  the reconcile exit code — it is an intended refusal, not a restore error
  (`REQ-RESTOREGUARD-02`).

## Scope and gate

- **Scope:** the restore/reconcile path. Unblocked-channel restore (the FEAT-0010
  crash-recovery replay of channels 0-5) is byte-for-byte unchanged
  (`REQ-RESTOREGUARD-04`); computed duty, cadence, live channel set, and control
  identity are unchanged (`REQ-RESTOREGUARD-05`).
- **Priority note:** unreachable under the shipped single-profile config, so this is
  the lowest-urgency of the four write-path rounds; it is promoted for correctness
  and guard parity, not because it is live-reachable today.
- **Measurement gate:** not crossed (`docs/MEASUREMENT_GATE.md`); startup/one-shot
  path, the change only *removes* a blocked-channel write, additive `reconcile.*`
  event, no schema break.
- **Implementation/verification** are authorized by this decision but are **staged
  for a Windows-host session** (Windows-only build, `CMAKE_RC_COMPILER`): the
  blocked-channel-skip, exit-code, in-process-restore-refusal, and
  unblocked-unchanged tests (call-recording / simulated writer + `blocked_channels`
  policy) must pass under `Test-LocalCI` before the spec §14 log is filled.
