# Decision: a corrupt pending-writes sidecar is quarantined and startup proceeds (Direction A)

**Project:** svg-mb-control
**Status:** Current (Accepted 2026-06-18)
**Owning feature:** `docs/features/FEAT-0012-startup-tolerates-corrupt-pending-writes-sidecar.md`
(`REQ-SIDECARRESIL-*`)
**Companion to:** `AGENTS.md`, `docs/WRITE_ORCHESTRATION.md`,
`docs/RUNTIME_HOME.md`, `docs/MEASUREMENT_GATE.md`,
`docs/features/FEAT-0008-watchdog-hung-worker-recovery.md`

## Context

At startup the worker reconciles the pending-write sidecar before the control loop
runs. `ReconcilePendingWrites` (`src/runtime/write_orchestrator.cpp:291-307`) reads
through `ReadPendingWrites` (`src/runtime/pending_writes.cpp:47-70`), which throws on
malformed JSON (`json_io.cpp:162-168`) or a missing required key
(`pending_writes.cpp:14-26`). On the throw it logs `reconcile.sidecar_read_failed`
and returns `1`; `app_main.cpp:303-306` returns that non-zero result and the worker
exits **before** the control loop starts. The corrupt file is never set aside, so
the next start re-reads the same bytes and re-fails — and the supervisor/watchdog
relaunch loop turns that into a self-perpetuating thrash with the control loop never
running and fans frozen at the last latched PWM. The same file is read with the
**opposite** tolerance by the steady loop (`control_loop.cpp:150-161`: log and
continue empty). Team review finding H2; static-verified, not runtime-reproduced.

## Options considered

- **A — quarantine the whole file, treat the reconcile set as empty.** Simplest;
  loses any still-parseable entries in the bad file.
- **B — per-entry skip, reconcile the parseable entries.** Preserves partially
  parseable records; more code in the parse loop and more failure-mode surface.

## Decision

**Adopt Direction A.** Reasons:

- It is the smallest correct change that breaks the thrash loop: treat a corrupt
  sidecar exactly as a *missing* one is already treated (`ReadPendingWrites` returns
  `{}` for absent, `pending_writes.cpp:51-53`) and as the steady loop already treats
  a parse failure (`control_loop.cpp:150-161`), so the startup reader's tolerance
  becomes consistent with the reader that already does it right (`REQ-SIDECARRESIL-01`).
- The corrupt-file trigger is uncommon (the atomic temp+rename write path leaves a
  complete file on an ordinary crash; corruption needs power-loss mid-write, an
  external edit, or a truncation), so the marginal value of salvaging partial entries
  (Direction B) is low against its added parse-loop complexity. B stays a later
  refinement if preserving partially-parseable records proves valuable.
- The accepted residual matches FEAT-0010's pattern: a baseline captured **only** in
  the now-corrupt file is not restored — but it was unreadable in any case, and the
  next worker start reasserts control on all channels (`REQ-SIDECARRESIL-04`).

**Quarantine, not delete (`REQ-SIDECARRESIL-02`):** rename the bad file aside to a
fixed `pending_writes.json.corrupt` sibling (overwritten on each new corruption,
keeping one most-recent sample) rather than deleting it, so the cause is inspectable.

**Observable, not silent (`REQ-SIDECARRESIL-03`):** reuse
`reconcile.sidecar_read_failed` and add one additive `reconcile.sidecar_quarantined`
event recording the original and quarantined paths; degrade runtime health so
`--health` reflects the lost recovery records. The degraded condition clears on the
next clean startup (the file is quarantined, so a clean restart self-heals).

## Scope and gate

- **Scope:** the startup sidecar-read/reconcile path only. The happy-path
  reconcile/restore, the sidecar **write** path and its atomic durability, the
  watchdog/supervisor relaunch policy, cadence, channels, and control identity are
  unchanged (`REQ-SIDECARRESIL-05`). This removes the *cause* of the thrash, not the
  relaunch mechanism (FEAT-0008).
- **Measurement gate:** not crossed (`docs/MEASUREMENT_GATE.md`); startup-read-path
  only, no fan write added, additive schema to `docs/RUNTIME_HOME.md`.
- **Implementation/verification** are authorized by this decision but are **staged
  for a Windows-host session** (Windows-only build, `CMAKE_RC_COMPILER`): the
  seed-a-corrupt-sidecar startup-proceeds / quarantine / degraded-health tests must
  pass under `Test-LocalCI` before the spec §14 log is filled. A runtime reproduction
  (seed a corrupt sidecar, observe the relaunch loop) would further strengthen §2.
