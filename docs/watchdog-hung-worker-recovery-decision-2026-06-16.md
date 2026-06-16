# Watchdog Hung-Worker Recovery — Decision & Implementation Plan — 2026-06-16

**Status:** Implemented (2026-06-16). Settled FEAT-0008 promotion gate 3; the
force-terminate escalation (D1–D3) landed in
`src/control/worker_force_terminate.{h,cpp}` + the `app_main.cpp` `--restart`
`stop_result == 2` branch, with C++ unit + Python suspend-based integration
tests. See FEAT-0008 §14 for the verification log and spec-vs-implementation
deltas (testable `ProcessTerminator` seam; PID-corroboration guard;
`NtSuspendProcess` test fixture instead of a controller ignore-stop mode).
**Owns:** `docs/features/FEAT-0008-watchdog-hung-worker-recovery.md` (`REQ-WATCHDOG-*`).
**Basis:** `docs/cpu-loop-survival-layer0-plan-2026-06-16.md` §3.2 (L0-A4) and the
verified gap in `docs/cpu-loop-stall-reproduction-findings-2026-06-16.md`.
**Scope guard:** this record captures the decision and the implemented v1 slice.
Future behavior expansion still requires the `AGENTS.md` Feature Intake Gate.

## 1. Context (the gap this settles)

Verified from source 2026-06-16: on `--restart` the handler calls
`RequestStopAndWait` (`src/control/control_supervisor.cpp:399-423`), which writes a
stop **sentinel file** and waits 15 s for the worker to report stopped
(`WaitForRuntimeStop`). A worker frozen hard enough never polls the sentinel, so the
wait returns **2**; `src/app/app_main.cpp:190-197` then does
`if (stop_result != 0) return stop_result;` **before** `start_requested = true`, so
no relaunch happens — and a grep of `src/` finds **no** `TerminateProcess`/force-kill.
Result: a genuinely hung worker is detected (`kStale` → exit 2 → `--restart`) but
never recovered.

## 2. Decisions

### D1 — Force-kill the worker first; the supervisor only if it does not self-exit

Kill the **worker** PID first. Because the stop sentinel is already set, when the
worker process dies the supervisor's `WaitForSingleObject` returns, it records the
exit, sees `stop_requested == true`, and **breaks its loop and exits cleanly**
(`control_supervisor.cpp:585-640`). So killing the worker normally brings the
supervisor down on its own. Only if the supervisor is **still alive** after a short
confirm window (it is itself wedged) do we force-kill `supervisor_pid` too.

*Rationale:* minimal blast radius; the supervisor loop is simple and not doing the
heavy sensor I/O that hangs, so it almost always exits itself. Rejected:
unconditionally killing both (kills a healthy supervisor that would have exited and
loses its clean shutdown bookkeeping).

### D2 — Reuse the existing 15 s graceful deadline; no new config key (v1)

The escalation triggers exactly on `RequestStopAndWait` returning 2 (the existing
`WaitForRuntimeStop` 15 s timeout). No new `control.json` key in v1.

*Rationale:* the 15 s graceful window already exists and is the natural trigger;
a configurable deadline is unneeded complexity for v1 and is listed as a future
option (§5). Keeps the change additive and small.

### D3 — Bounded, single-shot escalation with a PID-reuse guard; no loop

- **PID-reuse guard (required):** before terminating, resolve the target PID from
  `control_supervisor.json` (`last_worker_pid`, cross-checked against
  `control_runtime.json` `process_id`) **and confirm the live process image path is
  the release `svg-mb-control.exe`**. Never `TerminateProcess` a PID whose image does
  not match — guards against Windows PID reuse killing an unrelated process.
- **Bound:** one worker force-kill + a short confirm wait (≤ 3 s); then, only if the
  supervisor is still alive, one supervisor force-kill + ≤ 3 s confirm. No retry loop
  inside a single `--restart` invocation (the watchdog's 2-minute `ExecutionTimeLimit`
  must not be exceeded).
- **On failure to confirm exit:** emit `supervisor.force_terminate_failed`, do **not**
  set `start_requested`, return non-zero. The watchdog re-attempts on its next
  1-minute poll.

*Rationale:* force-kill is near-instant; a short confirm is enough. The guard and the
no-loop bound keep the escalation safe and watchdog-time-bounded.

## 3. Implementation plan (files, functions, sequence)

Implemented path, scoped to the `--restart` recovery route only (a plain `--stop`
is left graceful-only in v1):

1. **New helper** in `src/control/worker_force_terminate.{h,cpp}`:
   `EscalateForceTerminate(runtime_home, graceful_stop_result) -> bool` reads
   `SupervisorState` (`ReadSupervisorState`) for `last_worker_pid` /
   `supervisor_pid`, cross-checks the worker PID against `control_runtime.json`
   when available, verifies the live process image path matches the current
   `svg-mb-control.exe` image (`QueryFullProcessImageNameW`), calls
   `TerminateProcess`, and confirms exit with a bounded `WaitForSingleObject`.
   It emits `supervisor.worker_force_terminated`,
   `supervisor.supervisor_force_terminated`, or
   `supervisor.force_terminate_failed` via `AppendRuntimeEvent`. Return value is
   whether the runtime is confirmed gone.
2. **Wire it into the restart path:** in `app_main.cpp:190-197`, when
   `RequestStopAndWait` returns `2` (the hung-stop code, distinct from `1` =
   could-not-write-stop), call `EscalateForceTerminate`; on success set
   `start_requested = true` (relaunch proceeds); on failure return the error
   unchanged. `stop_result == 0` keeps the existing fast path untouched, satisfying
   REQ-WATCHDOG-04 (a worker that honors the stop is never force-killed).
3. **No change** to the worker, the tick loop, the sidecar gate, `RequestStopAndWait`
   itself (still used by `--stop`), or the supervisor crash-restart loop. The relaunch
   reuses the existing `LaunchDetachedLongRunningMode` path, whose startup runs
   `ReconcilePendingWrites` — the write-once recovery (REQ-WATCHDOG-03) is therefore
   already provided and only needs a test.

## 4. Test plan (REQ coverage)

- **REQ-WATCHDOG-01 (T, M):** an integration test suspends the real worker process
  with `NtSuspendProcess`, drives `--restart`, and asserts the worker PID is
  terminated and a fresh worker is launched. Manual (M): the live
  A4 repro from `docs/cpu-loop-stall-reproduction-protocol-2026-06-15.md` (hold a
  worker blocked > 15 s) shows recovery.
- **REQ-WATCHDOG-02 (T, R):** assert the `supervisor.worker_force_terminated` event
  is emitted with PID + reason; review the event against `docs/RUNTIME_HOME.md`.
- **REQ-WATCHDOG-03 (T, R):** seed an orphaned `pending_writes.json` entry, force-kill
  + relaunch, assert `ReconcilePendingWrites` restores the baseline; review vs
  `docs/WRITE_ORCHESTRATION.md` that ordering/recovery is unchanged.
- **REQ-WATCHDOG-04 (T, R):** a worker that honors the stop within 15 s is **not**
  force-killed (`stop_result == 0` fast path); and the PID-reuse guard refuses a PID
  whose image is not `svg-mb-control.exe`. Review the trigger gate and the bound.

Unit coverage lives in `tests/cpp/worker_force_terminate_tests.cpp`; integration
coverage lives in `tests/test_watchdog_force_terminate.py`. The test fixture uses
the real worker process rather than adding an ignore-stop test mode.

## 5. Open / future (not v1)

- Configurable force-kill grace period (vs the fixed 15 s) — add a `control.json` key
  only if 15 s proves wrong in practice.
- Apply the same escalation to a plain `--stop` after timeout (operator force-stop).
- Tighten the watchdog 1-minute poll / 10 s staleness (separate lever L0-C, deferred).

## 6. Invariants preserved (cross-check vs FEAT-0008 §4)

- **Live Runtime Safety:** force-kill is a process action on the already-authorized
  `--restart` path; no fan write; fans hold last PWM during the gap; the relaunched
  worker reasserts authority normally.
- **Write-once crash recovery:** a force-kill is a crash; `ReconcilePendingWrites`
  restores the baseline on relaunch. No new actuation; ordering unchanged.
- **Supervisor singleton:** terminate + confirm-gone before relaunch preserves the
  single-supervisor-per-runtime-home guarantee.
- **Schema stability:** additive event types only; no field/version change.

## 7. Risks & rollback

- *PID reuse* → mitigated by the image-path guard (D3); the escalation is a no-op if
  the guard fails.
- *Killing a recoverable-slow worker* → mitigated by triggering only on the 15 s
  `stop_result == 2` timeout, not on slowness (REQ-WATCHDOG-04).
- *Force-kill loop on a wedged machine* → mitigated by the single-shot bound (D3); the
  watchdog's own 1-minute cadence is the outer retry, and the supervisor backoff caps
  relaunch rate.
- *Rollback:* the change is contained to the `--restart` escalation; reverting the
  two edits restores the prior detect-but-don't-recover behavior with no schema or
  config migration.
