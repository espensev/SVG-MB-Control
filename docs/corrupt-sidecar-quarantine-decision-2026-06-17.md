# Corrupt pending-writes sidecar quarantine — Decision & Plan — 2026-06-17

**Status:** Current — decision settled 2026-06-17. Settles FEAT-0012 promotion
gate 3 and the §11 open decisions; authorizes the v1 implementation.
**Owns:** `docs/features/FEAT-0012-startup-tolerates-corrupt-pending-writes-sidecar.md`
(`REQ-SIDECARRESIL-*`).
**Basis:** the static-verified gap (team review finding H2): a corrupt
`pending_writes.json` makes the startup reconcile read throw
(`pending_writes.cpp` → `json_io.cpp`), `ReconcilePendingWrites` returns non-zero,
`app_main.cpp` treats it as fatal, and the watchdog relaunch re-reads the same bad
file = a permanent relaunch-thrash loop with the control loop never running. The
steady control loop already tolerates the same failure (`control_loop.cpp`).

## 1. Context

The startup reconcile read is the only sidecar reader that treats a corrupt file
as fatal; every other reader (the steady loop's `PendingWritesStore::Load` inside a
`try`/`catch`, and `ReadPendingWrites` for a *missing* file) tolerates it. This
decision makes the startup read tolerant too, breaking the thrash loop.

## 2. Decisions

### D-SIDECAR-1 — Direction A: quarantine the whole file, treat the set as empty
On any read/parse/shape failure the startup reconcile quarantines the corrupt file
and proceeds as if the sidecar were empty, letting the control loop start.

*Rationale:* the smallest correct change. The corrupt-file trigger is uncommon
(the atomic temp-file+rename write path leaves complete files; corruption needs
power-loss mid-write, external edits, or truncation), so per-entry salvage
(Direction B) is not worth the parse-loop complexity for v1. B remains a later
refinement if preserving partially-parseable records proves valuable.

### D-SIDECAR-2 — Fixed `pending_writes.json.corrupt` quarantine sibling, overwrite
The corrupt file is renamed to `<runtime_home>/pending_writes.json.corrupt`,
overwriting any prior quarantine (keeps one most-recent sample).
`std::filesystem::rename` replaces an existing target on both POSIX and Windows.

*Rationale:* deterministic and simple (testable, no timestamp non-determinism).
One most-recent corrupt sample is enough for forensics; timestamped retention of N
samples is an unneeded refinement for a rare condition.

### D-SIDECAR-3 — Additive `reconcile.sidecar_quarantined` event; tolerant read
A new additive event `reconcile.sidecar_quarantined` records the parse error and
the quarantine path. The former fatal `reconcile.sidecar_read_failed` + non-zero
return is removed from the startup read path (the read is now tolerant). The
duplicate sidecar read in `ReconcilePendingWrites` (it read via `ReadPendingWrites`
then `PendingWritesStore::Load()` again) is collapsed: the entries read once are
adopted into the store via a new `PendingWritesStore::Adopt`.

### D-SIDECAR-4 — Health degraded while the `.corrupt` artifact exists
The runtime health probe degrades (`sidecar_quarantined_present`, surfaced in the
`--health` JSON) while `pending_writes.json.corrupt` exists, so `--health` reflects
that recovery records were lost until an operator inspects and removes it.

*Rationale:* overrides the §11 "clears on next clean startup" default with the more
conservative "persists until acknowledged" — a lost-recovery-records signal should
not silently vanish on the next restart. Mirrors the existing
`pending_writes_unreadable` health-flag pattern.

## 3. Risks & mitigations
- **Quarantine rename fails (target locked / FS error).** The tolerant read still
  returns empty and startup still proceeds (the thrash loop is broken); the detail
  records the rename failure. The bad file remains and is re-quarantined next
  start. Safety (startup proceeds) holds; forensic preservation is best-effort.
- **A genuinely-pending baseline in the now-corrupt file is not restored.** Accepted
  residual (FEAT-0012 §4): the records were already unreadable; the next worker
  start reasserts control. Mirrors the FEAT-0010 accepted-residual pattern.
- **Happy path regression.** None: a parseable sidecar reconciles/restores exactly
  as before (REQ-SIDECARRESIL-04); only the failure path changes.

## 4. Rollback
Contained: revert the `ReadPendingWritesTolerant` quarantine + the
`ReconcilePendingWrites` tolerant call + the `Adopt` collapse + the
`sidecar_quarantined_present` health flag. No schema/config migration; the
`.corrupt` artifact is additive (absence reads as "no corruption seen").
