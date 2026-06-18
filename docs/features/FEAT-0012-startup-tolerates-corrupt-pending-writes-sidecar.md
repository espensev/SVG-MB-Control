# FEAT-0012: Startup tolerates a corrupt pending-writes sidecar

**Project:** svg-mb-control
**Status:** Accepted (2026-06-18)   **Version:** 0.2   **Updated:** 2026-06-18
**Namespace:** `REQ-SIDECARRESIL-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/WRITE_ORCHESTRATION.md`, `docs/RUNTIME_HOME.md`,
`docs/features/FEAT-0008-watchdog-hung-worker-recovery.md`,
`docs/features/FEAT-0010-write-actuation-sidecar-fault.md`
**Purpose:** a corrupt or unparseable `pending_writes.json` must not turn a
recoverable transient into a permanent worker-relaunch loop that never runs the
control loop, so a single bad recovery file cannot leave the fans frozen at the
last latched PWM while temperature rises.

> **Accepted (2026-06-18).** The maintainer accepted **Direction A** (quarantine
> the corrupt file and proceed as empty) — see the §9 decision record
> `docs/corrupt-pending-writes-startup-decision-2026-06-18.md`. All seven promotion
> gates pass; the spec is buildable. Implementation and verification are staged for
> a Windows-host session (this repo's build is Windows-only).

## 1. Summary

At startup the worker reconciles the pending-write sidecar before the control
loop runs: `app_main.cpp:298-307` calls `ReconcilePendingWrites`
(`src/runtime/write_orchestrator.cpp:291-307`), which calls `ReadPendingWrites`
(`src/runtime/pending_writes.cpp:47-70`). `ReadPendingWrites` parses the file
through `ReadJsonFile`, which throws `std::runtime_error` on malformed JSON
(`src/runtime/json_io.cpp:162-168`), and `PendingWriteEntryFromJson` throws via
`item.at(...)` on a missing required key (`pending_writes.cpp:14-26`). On that
throw, `ReconcilePendingWrites` logs `reconcile.sidecar_read_failed` and returns
`1` (`write_orchestrator.cpp:297-307`); `app_main.cpp:303-306` then returns that
non-zero result and the worker exits **before** the control loop starts. The
corrupt file is never quarantined or renamed aside, so the next worker start
re-reads the same bad bytes and re-fails identically. Because the supervisor
relaunch loop re-spawns the worker (`src/control/control_supervisor.cpp:618-668`)
and the watchdog scheduled task re-spawns the supervisor every
`IntervalMinutes` (default 1; `Install-SVG-MB-ControlWatchdogScheduledTask.ps1`),
the result is a self-perpetuating relaunch loop with the control loop never
running. This feature **proposes** making the startup reconcile read tolerate a
corrupt sidecar — quarantine the bad file and proceed as if empty, logging and
degrading health — so the loop starts and the thrash loop is broken.

## 2. Problem & motivation  *(promotion gate 1)*

This is a named code/contract gap, statically sourced from team review finding
H2 (`review/svg-mb-control-review-20260617-team-review.md`, High section,
"Corrupt/partial `pending_writes.json` → fatal startup reconcile → permanent
restart-thrash with fans frozen"). It is static-verified against the source,
**not** runtime-reproduced.

1. **The startup reconcile read is fatal on a parse failure.**
   `ReadPendingWrites` parses via `ReadJsonFile`, which re-throws as
   `std::runtime_error` on a malformed document
   (`src/runtime/json_io.cpp:162-168`), and `PendingWriteEntryFromJson` throws
   from `item.at("channel")` / `.at("baseline_duty_raw")` / etc. on a missing
   required key (`src/runtime/pending_writes.cpp:14-26`). A non-array `entries`
   value also throws (`pending_writes.cpp:60-62`). `ReconcilePendingWrites`
   catches the throw, logs `reconcile.sidecar_read_failed`, and `return`s `1`
   (`src/runtime/write_orchestrator.cpp:297-307`). `app_main.cpp:303-306` returns
   that non-zero `reconcile_result`, so the worker process exits before
   `RunControlLoop` is reached.

2. **The bad file is never set aside, so the failure is permanent.** The
   reconcile path neither renames nor truncates the corrupt sidecar on the
   failure branch (`write_orchestrator.cpp:297-307`). The next worker start reads
   the identical file at the same path
   (`PendingWritesSidecarPath`, `pending_writes.cpp:42-45`) and re-throws, so the
   transient becomes a steady fault.

3. **The relaunch loop converts that into a thrash loop — two cadences, one dead
   loop.**
   - *Warm path:* the first-launch give-up guard at
     `control_supervisor.cpp:618` requires `restart_count == 0u`. A worker that
     ran past the startup window then crashed or lost power mid-write has
     `restart_count >= 1`, which disarms that guard; the failed restart then
     falls through to `++restart_count` (`control_supervisor.cpp:642`) and
     re-spawns the worker with a capped backoff (`1u << restart_count`, clamped to
     60 s; `control_supervisor.cpp:647-648`). Each relaunch re-reads the corrupt
     file and re-fails fast, so the supervisor loops at up to a 60 s period
     indefinitely.
   - *Cold-start path:* on a fresh supervisor (`restart_count == 0u`), the
     give-up guard fires and the supervisor exits with the worker's exit code
     (`control_supervisor.cpp:618-633`); the watchdog scheduled task then relaunches
     a new supervisor every `IntervalMinutes` (default 1;
     `Install-SVG-MB-ControlWatchdogScheduledTask.ps1`), whose worker re-fails
     identically — a 1-minute-period dead loop.

4. **The same file is read with opposite fault tolerance elsewhere — the
   asymmetry shows the fatal branch is not the intended contract.** The steady
   control loop loads the same sidecar through `PendingWritesStore::Load`
   (`control_loop.cpp:147-161`) inside a `try`/`catch`: on a parse failure it logs
   `control_loop.sidecar_load_failed` and continues with an empty in-memory cache
   (`control_loop.cpp:150-161`). The startup reconcile reads the identical file
   and treats the identical failure as fatal. One reader tolerates a corrupt
   sidecar; the other does not.

**Fail direction (from H2):** unsafe-leaning. While the control loop never runs,
the fans hold the last PWM the crashed worker latched (`restore_on_exit=false`,
`third_party/SVG-MB-SIO/src/sio_fan_writer.cpp:94` per H2), so they cannot ramp on
a later thermal excursion. The hardware backstop (the SMU 95 °C throttle,
`docs/features/FEAT-0008-watchdog-hung-worker-recovery.md` §2) still applies, but
the controller provides no software recovery.

**Trigger is narrow, reachability is full.** The atomic temp-file + rename write
path (`WriteJsonFileAtomic` via `ReplaceFileWithTemp`,
`src/runtime/json_io.cpp:33-69`) means an ordinary crash leaves a *complete,
parseable* file, so the corrupt-file trigger requires power-loss/truncation
mid-write, an externally-edited or partially-written file, a missing required key,
a non-array `entries`, or a type mismatch. The condition is therefore uncommon —
but once present it is fully reachable and self-perpetuating, which is why the
fail direction (a permanent dead loop) is the concern rather than the probability.

## 3. Goals & non-goals

**Goals**
- A corrupt or unparseable `pending_writes.json` must not prevent the worker from
  reaching the control loop at startup (the relaunch loop must be broken).
- The corrupt file must be preserved for forensics (quarantined, not silently
  deleted), so the cause can be inspected after the fact.
- A corrupt-sidecar startup must be observable (event + degraded health), not a
  silent recovery.
- Make the startup reconcile read's fault tolerance consistent with the steady
  control loop's existing tolerance (`control_loop.cpp:150-161`).

**Non-goals**
- No change to the happy path: when the sidecar parses, reconcile/restore runs
  exactly as today (`write_orchestrator.cpp:308-360`), restoring captured
  baselines.
- No change to the watchdog or supervisor relaunch policy itself
  (`docs/features/FEAT-0008-watchdog-hung-worker-recovery.md`); this feature
  removes the *cause* of the thrash, not the relaunch mechanism.
- No change to how the sidecar is written or to the atomic-write durability
  guarantee (`src/runtime/json_io.cpp:33-69`).
- Does not address the neighboring write-path findings (the sidecar-persist veto
  over the live fan write — `docs/features/FEAT-0010-write-actuation-sidecar-fault.md`;
  the restore/reconcile blocked-channel-guard bypass); those are cross-referenced
  (§12) and out of scope here.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| No fan write / authority change outside an explicit live task | `AGENTS.md` §Live Runtime Safety | The change is in the startup reconcile *read* path; it issues no fan write. Proceeding past a corrupt sidecar lets the already-authorized control loop reassert authority through its normal startup path, exactly as it does today when the file is absent (`ReadPendingWrites` already returns `{}` for a missing file, `pending_writes.cpp:51-53`). |
| Write-once crash recovery: never `{durable hardware override + no durable baseline record}` | `docs/WRITE_ORCHESTRATION.md`, `pending_writes.cpp`, `app_main.cpp`; `FEAT-0008` §4 | A corrupt sidecar means the baseline record is *already* unreadable — the system is already in the no-readable-baseline state before this feature runs. Quarantining and proceeding does not create that state; it recovers from it, letting the loop reassert control on all channels. Thrashing instead guarantees both no recovery and no control. **Accepted residual:** a genuinely-pending baseline that was captured only in the now-corrupt file is not restored — but it was unrecoverable in any case, and the next worker start reasserts control. This mirrors the `FEAT-0010` accepted-residual pattern. |
| Shipped 250 ms cadence / channel set / input strategy is the measured baseline | `docs/MEASUREMENT_GATE.md` | Startup-read-path only; cadence, channels, and input strategy are unchanged, so the gate baseline does not move. |
| Runtime sidecar / status / event schema stays backward-compatible | `docs/RUNTIME_HOME.md` | The proposal reuses the existing `reconcile.sidecar_read_failed` event (`write_orchestrator.cpp:300-303`), adds at most one additive quarantine event and a `.corrupt` sidecar artifact, and degrades the existing health-state vocabulary. No existing field, file, or schema version changes; an absent quarantine artifact reads as "no corruption seen." |
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | The change is confined to in-repo runtime code (`src/runtime/`, `src/app/`); no external dependency. |

## 5. Behavior specification

Behavior is **proposed** (not yet implemented, and a direction is not yet
chosen — see §11). It would live in or near the startup reconcile read
(`src/runtime/write_orchestrator.cpp` `ReconcilePendingWrites` and/or
`src/runtime/pending_writes.cpp` `ReadPendingWrites`), with health degradation in
the startup/runtime-health path (`src/runtime/runtime_health.cpp`).

- **Happy path unchanged.** When the sidecar parses, reconcile/restore runs
  exactly as today: read entries, restore each captured baseline, then rewrite the
  sidecar (`write_orchestrator.cpp:308-360`).

- **Corrupt sidecar must not be fatal.** When the startup read throws (malformed
  JSON, missing required key, non-array `entries`, or type mismatch), the
  reconcile step must **not** return non-zero and abort the worker. The proposed
  direction is to treat the corrupt file as if it contained no recoverable
  entries and let startup proceed to the control loop, consistent with how a
  *missing* file is already handled (`ReadPendingWrites` returns `{}`,
  `pending_writes.cpp:51-53`) and with the steady loop's existing tolerance
  (`control_loop.cpp:150-161`).

- **Quarantine, not delete.** The proposed direction preserves the corrupt file
  for forensics by renaming it aside (for example to a `pending_writes.json.corrupt`
  or timestamped sibling) rather than overwriting or deleting it, so the cause can
  be inspected. The exact naming and retention is an open decision (§11).

- **Observable, not silent.** A corrupt-sidecar startup must emit a structured
  event — reusing the existing `reconcile.sidecar_read_failed`
  (`write_orchestrator.cpp:300-303`) and/or adding an additive quarantine event —
  and degrade the runtime health state so `--health` reflects that recovery
  records were lost. It must not be a silent recovery.

- **Crash recovery preserved on the common path.** The captured-baseline
  crash-recovery guarantee (`docs/WRITE_ORCHESTRATION.md` Runtime Flow) is
  unchanged whenever the sidecar parses. The only loss is for the corrupt-file
  case, where the records were already unreadable (the §4 accepted residual).

- **Direction not yet chosen.** Two candidate directions cover this behavior and
  are deliberately left open (§11): (A) quarantine the whole file and treat the
  reconcile set as empty (simplest; loses any still-parseable entries); (B) skip
  only the unparseable entries per-entry and reconcile the rest (preserves
  parseable records; more code in the parse loop). This spec does not pick one.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-SIDECARRESIL-01 | A corrupt or unparseable `pending_writes.json` at startup must not cause the worker to exit before the control loop runs: the startup reconcile must proceed (treating the unrecoverable records as empty) rather than returning a non-zero result that aborts the worker. |
| REQ-SIDECARRESIL-02 | On a corrupt-sidecar startup the bad file must be preserved for forensics (quarantined by renaming it aside, e.g. a `.corrupt` sibling) rather than silently deleted or overwritten in place. |
| REQ-SIDECARRESIL-03 | A corrupt-sidecar startup must be observable: it must emit a structured runtime event (reusing `reconcile.sidecar_read_failed` and/or an additive quarantine event) and degrade the runtime health state so `--health` reflects that recovery records were lost. It must not be a silent recovery. |
| REQ-SIDECARRESIL-04 | The captured-baseline crash-recovery guarantee must be unchanged whenever the sidecar parses; the loss of recovery records on the corrupt-file path is an accepted residual recorded in §4 (the records were already unreadable). |
| REQ-SIDECARRESIL-05 | The change must be confined to the startup sidecar-read/reconcile path: the happy-path reconcile/restore, the sidecar write path and its atomic-write durability, the watchdog/supervisor relaunch policy, the 250 ms cadence, the channel set, and the control-computation identity are unchanged, and any new status/health field or event is additive to `docs/RUNTIME_HOME.md`. |

## 7. Data / schema deltas

- New/changed fields: at most one additive quarantine event type (for example
  `reconcile.sidecar_quarantined`, recording the original and quarantined paths);
  reuse of the existing `reconcile.sidecar_read_failed` event
  (`write_orchestrator.cpp:300-303`); reuse of the existing health-state
  vocabulary (`degraded`) for the corrupt-sidecar condition. A `.corrupt`
  sidecar artifact may appear in the runtime home (additive; absence reads as "no
  corruption seen").
- Config impact (`config/control.*.json`, `config/machines/*.json`): none.
- Schema/version impact: additive only; update `docs/RUNTIME_HOME.md` (any new
  event + the health-degradation trigger + the `.corrupt` artifact) and
  `docs/WRITE_ORCHESTRATION.md` (reconcile failure-path behavior) at
  implementation. No existing runtime-home file, archive, or config becomes
  invalid.

## 8. CLI / config / operator surface deltas

- `--health` / `--status` reflect the degraded health state after a
  corrupt-sidecar startup (read-only). No new operator write action.
- No new CLI subcommand or flag. UI is out of scope (`docs/MEASUREMENT_GATE.md`).
- Update `README.md` only if the `--health`/`--status` field list it documents
  changes; otherwise the doc updates are `docs/RUNTIME_HOME.md` and
  `docs/WRITE_ORCHESTRATION.md` per `AGENTS.md` §Change Checklist.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| `docs/corrupt-pending-writes-startup-decision-2026-06-18.md` | Direction A (quarantine whole file, proceed empty) vs. B (per-entry skip): **A accepted**; quarantine to a fixed `pending_writes.json.corrupt` sibling; reuse `reconcile.sidecar_read_failed` + add `reconcile.sidecar_quarantined`; degraded health clears on the next clean startup. | Accepted 2026-06-18 (current) |

The decision record `docs/corrupt-pending-writes-startup-decision-2026-06-18.md`
(Accepted 2026-06-18) settles Direction A and the quarantine / event / health
details; implementation is authorized, staged for a Windows-host build/verify.

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-SIDECARRESIL-01 | T | `.\scripts\Test-LocalCI.ps1` test: seed a malformed `pending_writes.json` (and separately a missing-key / non-array variant) in a temp runtime home, run the startup reconcile, and assert it proceeds (does not return the abort code) so the control loop is reachable. |
| REQ-SIDECARRESIL-02 | T | Test: after the corrupt-sidecar startup, the original bytes are preserved under a quarantine sibling path and the live `pending_writes.json` no longer blocks a clean start. |
| REQ-SIDECARRESIL-03 | T, R | Test asserts the event is emitted and the runtime health state degrades on the corrupt-sidecar path; review vs `docs/RUNTIME_HOME.md` (additive event + health trigger). |
| REQ-SIDECARRESIL-04 | T, R | Test: a parseable sidecar still reconciles/restores the captured baseline unchanged (happy path); review vs `docs/WRITE_ORCHESTRATION.md` Reconciliation that the common-path guarantee is preserved. |
| REQ-SIDECARRESIL-05 | R | Review vs `docs/CONTROL_PIPELINE_MATH.md`, `docs/MEASUREMENT_GATE.md`, and `FEAT-0008`: change confined to the startup read/reconcile path; write path, durability, relaunch policy, cadence, channels, and control identity unchanged; status/event additive. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

| Decision | Needed before | Current default (proposed, not decided) |
|---|---|---|
| Direction A (quarantine the whole file, treat the reconcile set as empty) vs. Direction B (skip only unparseable entries per-entry and reconcile the rest). | implementation | Lean A for v1 (simplest; the corrupt-file trigger is uncommon and a whole-file quarantine is the smallest correct change); B is a later refinement if preserving partially-parseable records proves valuable. Not decided. |
| Quarantine artifact naming and retention (fixed `.corrupt` sibling vs. timestamped; overwrite a prior quarantine vs. keep N). | implementation | A fixed `pending_writes.json.corrupt` sibling, overwritten on each new corruption (keeps one most-recent sample). Not decided. |
| Reuse `reconcile.sidecar_read_failed` only, or add a `reconcile.sidecar_quarantined` event. | implementation | Reuse the existing event and add one additive quarantine event recording the original and quarantined paths. Not decided. |
| Whether the degraded-health condition clears on the next clean startup or holds for a debounce window. | implementation | Clears on the next clean startup (the corrupt file is quarantined, so a clean restart self-heals). Not decided. |

## 12. Measurement gate & dependencies

- **Measurement gate:** not crossed. The change is startup-read-path only; it does
  not change cadence, live channels, or mixed-input strategy, and adds no term to
  the control identity, so no characterization evidence is required before
  implementation (`docs/MEASUREMENT_GATE.md`).
- **Depends on:** the startup reconcile read (`src/runtime/write_orchestrator.cpp`
  `ReconcilePendingWrites`), the sidecar reader (`src/runtime/pending_writes.cpp`
  `ReadPendingWrites`), and the runtime-health assessment
  (`src/runtime/runtime_health.cpp`). Relies on but does not change the supervisor
  relaunch loop (`src/control/control_supervisor.cpp:618-668`) or the watchdog
  scheduled task (`Install-SVG-MB-ControlWatchdogScheduledTask.ps1`).
- **Cross-references but does not depend on:** the sidecar-persist veto over the
  live fan write (`docs/features/FEAT-0010-write-actuation-sidecar-fault.md`) and
  the restore/reconcile blocked-channel-guard bypass
  (`third_party/SVG-MB-SIO/src/svg_mb_sio.cpp` restore), which are separate
  findings.
- **Build/test impact:** new tests under `tests/` (C++ and/or pytest) that seed a
  corrupt sidecar and assert startup proceeds, quarantines, and degrades health;
  doc updates to `docs/RUNTIME_HOME.md` and `docs/WRITE_ORCHESTRATION.md` per
  `AGENTS.md` §Change Checklist. No `docs/CONTROL_PIPELINE_MATH.md` change.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from a named code/contract gap (§2 — H2, static-verified against `write_orchestrator.cpp:297-307`, `pending_writes.cpp:14-26,47-70`, `json_io.cpp:162-168`, `app_main.cpp:298-307`, `control_supervisor.cpp:618-668`; the asymmetry vs `control_loop.cpp:150-161`).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [x] 3. Required design decision record(s) written and marked current — `docs/corrupt-pending-writes-startup-decision-2026-06-18.md` (Direction A), Accepted 2026-06-18 (§9).
- [x] 4. Concrete `REQ-SIDECARRESIL-*` IDs assigned from the namespace (§6).
- [x] 5. Verification mapped to real checks (§10 — `Test-LocalCI` / review) and mirrored in `docs/TRACEABILITY.md` (the `REQ-SIDECARRESIL-*` rows are present). The tests themselves are authored at implementation on a Windows host.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline (startup-read-path only; no fan write added; additive schema).
- [x] 7. Doctrine check: current behavior claims grounded with file:line; proposed behavior labeled as proposed; `must`/`should`/`is` used per `CLAUDE.md`; no undefined terms or unqualified vague adjectives.

> Promoted to Accepted 2026-06-18: Direction A (quarantine + proceed empty) is
> accepted (§9 decision record) and the `REQ-SIDECARRESIL-*` verification is
> mirrored in `docs/TRACEABILITY.md`, so promotion gates 3 and 5 pass with the rest.
> The hazard is static-verified, not runtime-reproduced; a reproduction (seed a
> corrupt sidecar, observe the relaunch loop) would further strengthen §2.
> Implementation and verification are staged for a Windows-host session.

## 14. Verification log  *(fill in after the feature is built — Accepted 2026-06-18, not yet implemented)*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-SIDECARRESIL-01 | | | |
| REQ-SIDECARRESIL-02 | | | |
| REQ-SIDECARRESIL-03 | | | |
| REQ-SIDECARRESIL-04 | | | |
| REQ-SIDECARRESIL-05 | | | |

**Spec vs. implementation deltas:** none yet (not implemented).
