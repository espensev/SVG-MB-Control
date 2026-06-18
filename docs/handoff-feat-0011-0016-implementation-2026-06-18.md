# Handoff: implement FEAT-0011..0016 + Bucket B tests (Windows-host session)

**Date:** 2026-06-18
**Branch / PR:** `claude/great-darwin-pqzn1s` / PR #9 (draft, docs-only)
**Status:** specs Accepted, decision records written, **no product code yet**.
**Why this handoff exists:** the work below requires the Windows-only build
toolchain to compile and verify; it was prepared on a Linux cloud container that
cannot build the project (see §0).

---

## 0. Environment constraint (read first)

This repo builds and tests **only on Windows**:

- `CMakeLists.txt` is Windows-targeted — `if(MSVC)` settings and `if(WIN32)`
  compiling `resources/svg_mb_control.rc`. A non-Windows CMake configure aborts
  with `No CMAKE_RC_COMPILER could be found`.
- The source is Win32-specific: `windows_lean.h`, `CreateFileW` /
  `FILE_APPEND_DATA`, a PawnIO kernel driver, Super I/O LPC port I/O, AMD SMN
  register reads. It does not target Linux, and the live hardware (NCT6701 fan
  controller, AMD sensors) is not present off the deployed machine.
- The harness is PowerShell: `scripts\Test-LocalCI.ps1` → `Build-Release.ps1`.

Only the **cross-platform Python lane** runs off-Windows (that is why
`tests/test_feature_specs.py` and the analyze tests pass in CI-less prep).

**To finish this work you need a Windows host** with MSVC + the Windows SDK (for
`rc.exe`), or a `windows-latest` CI runner (see §3).

---

## 1. What already landed on this branch (docs only)

- **Issue #4 specs:** `FEAT-0015` (event-JSONL retention, `REQ-EVENTRET-*`) and
  `FEAT-0016` (analyze-DB run-purge, `REQ-DBRETAIN-*`).
- **All six write-path/disk specs promoted to Accepted** with dated decision
  records (`docs/*-decision-2026-06-18.md`): FEAT-0011..0016.
- Registry (`docs/features/README.md`) + `docs/TRACEABILITY.md` updated;
  `tests/test_feature_specs.py` green (5/5).
- Backlog hygiene: HR-2/HR-1 retired (resolved by FEAT-0008); W7-1 noted.
- Doc drift: `CONTROL_PIPELINE_MATH.md` §6.1/§8.2 aligned to the
  integrate-then-clamp code (DRIFT-1).

Each Accepted spec's §14 verification log is empty and its `TRACEABILITY.md` §3
result is `pending`; both are filled at the end of implementation (§4).

---

## 2. Per-spec implementation targets

Each block: the accepted decision, the code site(s), the requirements, the tests
to add, and the docs to update. The decision record is the source of truth for
the direction; the spec is the source of truth for behavior and acceptance.

### FEAT-0011 — open breaker must not block a rising cooling command
- **Decision:** `docs/breaker-rising-cooling-demand-decision-2026-06-18.md` —
  accept the rising-demand bypass, bounded by a **margin above
  `last_issued_pct` + a per-channel probe-rate limit**, fixed in code.
- **Code:** the breaker gate `src/control/channel_write.cpp:307`
  (`TryApplyChannelSetpoint`); per-channel state in
  `src/control/control_runtime_context.h` (add a `last_breaker_probe_tick`-style
  field if the probe-rate bound needs it). Self-heal/close reuses the existing
  `NoteSuccessfulChannelWrite`; failure reuses `HandleChannelWriteFailure`.
- **Requirements:** `REQ-COOLWRITE-01..05`.
- **Tests** (C++, `tests/cpp/channel_write_tests.cpp`, drive with
  `src/hardware/simulated_fan_writer.cpp`): open-breaker **rising** command fires
  `ApplyDuty`; **down-or-equal** stays suppressed; `safety_override` bypass
  unchanged; bypass success closes the breaker (`circuit_breaker_closed`);
  bypass failure leaves it open with `consecutive_write_failures` correct; bound
  prevents an every-tick retry against a persistently-failing actuator.
- **Docs:** `docs/WRITE_ORCHESTRATION.md` (breaker self-heal);
  `docs/RUNTIME_HOME.md` if a probe-rate status field is added.

### FEAT-0012 — startup tolerates a corrupt pending-writes sidecar (Direction A)
- **Decision:** `docs/corrupt-pending-writes-startup-decision-2026-06-18.md` —
  **quarantine** the corrupt file to a fixed `pending_writes.json.corrupt`
  sibling, **proceed as empty**, reuse `reconcile.sidecar_read_failed` + add
  `reconcile.sidecar_quarantined`, **degrade health** (clears on next clean start).
- **Code:** `src/runtime/write_orchestrator.cpp` (`ReconcilePendingWrites`,
  ~291-307) and/or `src/runtime/pending_writes.cpp` (`ReadPendingWrites`, 47-70);
  health in `src/runtime/runtime_health.cpp`. On the parse throw, **do not**
  return the abort code; rename the file aside and continue.
- **Requirements:** `REQ-SIDECARRESIL-01..05`.
- **Tests** (`tests/`): seed a malformed / missing-key / non-array
  `pending_writes.json`; assert startup proceeds (no abort code); the original
  bytes survive under the quarantine sibling; the event fires and health
  degrades; a parseable sidecar still reconciles/restores unchanged (happy path).
- **Docs:** `docs/RUNTIME_HOME.md` (event + health trigger + `.corrupt`
  artifact); `docs/WRITE_ORCHESTRATION.md` (reconcile failure path).

### FEAT-0013 — source-aware CPU dropout enters safe mode
- **Decision:** `docs/source-aware-primary-dropout-decision-2026-06-18.md` —
  count a CPU dropout (CPU previously available, now NaN while GPU available)
  toward `consecutive_sensor_failures`; after 3 misses **force `kSafeModeFanDuty`
  + `safety_override`**; v1 CPU-only; arm on the first CPU-available tick.
- **Code:** `src/control/channel_evaluator.cpp` (`SelectPrimaryCurveInput`
  219-234, `EvaluatePrimarySetpoint` 268-276 — stop resetting the counter on the
  GPU fallback when CPU was available); new per-channel state
  (`cpu_input_was_available`) in `src/control/control_runtime_context.h`.
- **Requirements:** `REQ-SRCSAFE-01..05`.
- **Tests** (`tests/cpp/channel_write_tests.cpp`, sibling of `:251`
  `TestEvaluatorSetsSafetyOverrideOnSensorFailure`): CPU available→dropped while
  GPU stays increments the counter (no reset); three dropout ticks set
  `safety_override` + `kSafeModeFanDuty`; `FailureDetected` on trip /
  `Recovered` on CPU return; a never-had-CPU GPU-led channel does not trip.
- **Docs:** `docs/RUNTIME_HOME.md` (status field + event); `docs/CONTROL_LOOP.md`
  (source-aware dropout behavior). No `CONTROL_PIPELINE_MATH.md` math change.

### FEAT-0014 — reconcile/restore honor the blocked-channel guard
- **Decision:** `docs/reconcile-restore-blocked-channel-guard-decision-2026-06-18.md`
  — **Control-layer pre-check** in `ReconcilePendingWrites`/`RunWriteOnce`,
  **retain** the skipped entry, **explicit restore-time check**, emit
  `reconcile.restore_skipped_blocked`; no vendored (`third_party/SVG-MB-SIO`)
  change in v1.
- **Code:** `src/runtime/write_orchestrator.cpp` (restore loop 341-361; the
  `RunWriteOnce` restore). Consult `RuntimeWritePolicyBlocksChannel` +
  `writes_enabled` before `RestoreSavedState`.
- **Requirements:** `REQ-RESTOREGUARD-01..05`.
- **Tests** (`tests/`, call-recording / simulated writer + `blocked_channels`
  policy): blocked channel skipped (no `RestoreSavedState`); skip emits the event
  and keeps exit code 0; `RunWriteOnce` restore refuses a blocked channel;
  unblocked-channel restore byte-for-byte unchanged (FEAT-0010 regression guard).
- **Docs:** `docs/WRITE_ORCHESTRATION.md` (Reconciliation); `docs/RUNTIME_HOME.md`
  if the retain-vs-clear choice touches sidecar lifecycle.
- **Note:** lowest urgency — not reachable by the shipped single-profile config.

### FEAT-0015 — event JSONL retention bound (A+B)
- **Decision:** `docs/event-log-retention-decision-2026-06-18.md` — **rotate**
  the event JSONL on the `log_rotate_hours`/`log_retain_days` window **and**
  **reduce** routine `control_loop.write_applied` persistence, always keeping
  `warning`+ and lifecycle events; single atomic append preserved; absent-key
  default = current behavior; confirm/bound the mid-file NUL torn-write cause.
- **Code:** `src/runtime/runtime_event_log.cpp` (`AppendRuntimeEvent` 210;
  severity via `InferSeverity` 156; the share-flag handling at 28-40 anticipates
  the rotator); path resolution `src/runtime/runtime_paths.cpp`. Model the
  rotation on `RuntimeCsvLogger` (`src/runtime/runtime_csv_archive.cpp`,
  `MaybeRotate` / retention at 403/451).
- **Requirements:** `REQ-EVENTRET-01..05`.
- **Tests** (`tests/`): write past the bound → rotated/capped; whole NDJSON lines
  across a rotation boundary (no split/interleave); a `warning`/`error` is kept
  while routine `info` `write_applied` is reduced/rotated; absent config key
  preserves current append and `CachedEventCount` still parses.
- **Docs:** `docs/RUNTIME_HOME.md`, `docs/RUNTIME_LOGGING_AND_EVALUATION.md`.

### FEAT-0016 — analyze-DB run-purge + reclaim
- **Decision:** `docs/analyze-db-run-purge-decision-2026-06-18.md` — **age-based
  `--db-retain-days` purge inside `analyze prune`**, cascade-delete old `runs`
  under `PRAGMA foreign_keys = ON`, **one-time `VACUUM` only when rows were
  deleted**, **explicit W7-1 zero-retain guard** (zero = disabled, logged).
  Size/run-count cap deferred.
- **Code:** `src/analyze/analyze_prune.{h,cpp}` (extend `PruneOptions` +
  `RunAnalyzePrune`), `src/analyze/analyze_cli.cpp:41` (the flag), a bounded
  `DELETE FROM runs WHERE session_start < cutoff` helper in
  `src/analyze/analyze_ingest_db.cpp` (next to the idempotent-reingest deletes at
  67/84); cascade is already declared in `analyze_db.cpp:41,108,113,147,175`.
- **Requirements:** `REQ-DBRETAIN-01..05`.
- **Tests** (`tests/test_analyze_ingest.py` sibling): ingest runs spanning the
  bound, purge, assert out-of-bound deleted / in-bound retained; no `tick_*` /
  `events` row references a deleted `run_id` (cascade fired); `page_count` drops
  after a deleting purge and **no** VACUUM when nothing was deleted; retained
  runs still de-duplicate on re-ingest; dry-run vs `--apply`.
- **Docs:** `docs/RUNTIME_HOME.md`, `README.md` (analyze maintenance workflow).
- **W7-1 fold-in:** while here, guard the runtime CSV path zero-config trap —
  `log_retain_days == 0` / `log_rotate_hours == 0` silently disable pruning /
  rotation (`src/runtime/runtime_csv_archive.cpp:403,452`). Treat zero as
  "disabled" explicitly (logged), not a silent no-op.

**Suggested order** (risk-ascending): FEAT-0016 (offline analyze, no live path) →
FEAT-0015 (logging-side) → FEAT-0012 (startup read path) → FEAT-0013 → FEAT-0011
(both control-critical) → FEAT-0014 (lowest urgency).

---

## 3. Bucket B — no-decision quick wins (not feature-gated)

These need no FEAT spec (`AGENTS.md`: defect/test/doc work is exempt). The C++
ones still need a Windows build to verify.

- **F3 (C++ test):** the incident-proven SIO init-retry (5×250 ms) +
  transient-read retry (3×75 ms) have **zero** automated test. Add coverage.
- **W5-1 (C++ test):** `src/control/low_band_integrator.cpp`
  (`UpdateLowBandState`) has no unit test.
- **W3-3 (C++ test):** `duty_pct→duty_raw` (verified correct, `fan_sio.cpp:830`)
  has no regression test.
- **F2 / F5 (C++ tests):** hold-expiry `kRestoreFailed` (non-timeout) branch and
  `pending_writes_unreadable→kFailed` precedence are untested.
- **DRIFT-2 (doc only — not Windows-gated, can be done anywhere):**
  `docs/CONTROL_LOOP.md` lists per-channel fields as required that the loader
  treats as optional. Align the doc to the loader.

---

## 4. Closeout checklist (per spec, after the code lands)

Follow the `AGENTS.md` Change Checklist:

1. `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` → CTest + Python lanes green.
2. Fill the spec's **§14 verification log** (result / evidence / date) per `REQ-*`.
3. Flip `docs/TRACEABILITY.md` §3 results `pending → pass` and the §2 status
   `Accepted → Implemented` for that spec.
4. Update the mode/runtime docs the change touches (`RUNTIME_HOME.md`,
   `WRITE_ORCHESTRATION.md`, `CONTROL_LOOP.md`,
   `RUNTIME_LOGGING_AND_EVALUATION.md`, `README.md`).
5. `tests/test_feature_specs.py` must stay green (it enforces registry /
   requirement / traceability / gate / verification-log consistency).
6. Add a `docs/PATH_NOTES.md` entry.

---

## 5. Windows CI runner (added in this branch)

`.github/workflows/ci-windows.yml` was added on this branch. It runs the
documented `scripts\Test-LocalCI.ps1 -KeepBuildDir` on `windows-latest` for every
PR and `main` push: it builds the C++ and runs the **CTest unit lane**
(boost_stage, control_math, channel_write, low_band_integrator, analyze_* — all
pure logic, no hardware) plus the hermetic Python lane. The MSVC environment is
bootstrapped by the repo's own `scripts\Build.VsEnv.ps1` (vswhere + VsDevShell),
so no separate compiler-setup step is needed (VS 2022 + the VC tools are
preinstalled on the runner image).

That lets FEAT-0011..0016 be **built and verified through CI** from any session,
not only on a local Windows box. The live, hardware-dependent paths (real fan
writes, AMD/SIO reads) still cannot run on a cloud runner and stay manual-`M`
evidence on the deployed machine.

**One-time:** confirm GitHub Actions is enabled for the repo (it had 0 check runs
before this), and check the first run — if `Build-Release.ps1` needs Ninja or a
component the runner image lacks, add the install step (kept out by default to
avoid hand-rolling toolchain setup against `AGENTS.md`).

---

## 6. Provenance

- Specs: `docs/features/FEAT-0011..0016*.md` (Accepted 2026-06-18).
- Decisions: `docs/*-decision-2026-06-18.md` (Current).
- Map: `docs/TRACEABILITY.md` §2/§3. Registry: `docs/features/README.md`.
- Issue #4 evidence: `docs/discovery-runtime-disk-growth-2026-06-14.md`.
- Branch `claude/great-darwin-pqzn1s`, PR #9.
