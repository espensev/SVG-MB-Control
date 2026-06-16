# FEAT-0008: Watchdog hung-worker recovery (force-kill escalation)

**Project:** svg-mb-control
**Status:** Implemented   **Version:** 0.3   **Updated:** 2026-06-16
**Namespace:** `REQ-WATCHDOG-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/WRITE_ORCHESTRATION.md`
**Purpose:** when the watchdog detects a stale (hung) worker but the graceful
stop times out, escalate to a forced termination and relaunch so a truly frozen
control loop is actually recovered, not merely detected.

## 1. Summary

Before this feature, the watchdog *detected* a hung worker (health exit code 2,
`kStale`) and issued `--restart`, but if the worker was frozen hard enough not to
honor the graceful stop within `RequestStopAndWait`'s deadline, the controller
returned **without relaunching** and no force-kill existed, so the loop stayed
down until the next event (reboot, manual restart). This feature adds a bounded
force-terminate escalation on the restart path: after the graceful stop times
out, force-terminate the worker (and supervisor if still present), then relaunch
— recovering the `{worker alive but frozen}` case the old path detected but could
not clear.

## 2. Problem & motivation  *(promotion gate 1)*

The pre-implementation source review on 2026-06-16 found: on `--restart` the
handler called `RequestStopAndWait`
(`src/control/control_supervisor.cpp:399-423`, 15 s deadline) and then
`app_main.cpp:190-197` did `if (stop_result != 0) return stop_result;`
**before** `options.start_requested = true`. So a stop that times out aborts the
relaunch. A grep of `src/` at that point found **no** `TerminateProcess` /
force-kill in the watchdog or supervisor paths. The watchdog scheduled task
therefore detected a hung worker (`kStale` -> exit 2 -> `--restart`) but could
not recover one that would not honor the stop sentinel.

This is the "everything stopped updating" freeze class from the 06-09 incident
(`docs/cpu-peak-temp-excursion-2026-06-09.md`). The 2026-06-16 reproduction
(`docs/cpu-loop-stall-reproduction-findings-2026-06-16.md`) only exercised the
*recoverable* case — the worker honored the graceful stop — so the non-recovery
gap is currently unguarded. While a worker is down, fans hold the last PWM
(SuperIO/EC) with the SMU 95 °C throttle as the hardware backstop; recovery still
matters so the loop resumes tracking rather than holding a stale duty indefinitely.

## 3. Goals & non-goals

**Goals**
- On the existing `--restart` path, escalate to a bounded forced termination when
  the graceful stop times out, then relaunch.
- Make the escalation observable in the runtime event log.
- Keep the forced termination inside the existing write-once crash-recovery path
  (a force-kill is handled like a crash).

**Non-goals**
- No change to detection (the `kStale`/10 s staleness verdict and the 1-minute
  watchdog cadence are out of scope here; see `docs/cpu-loop-survival-layer0-plan-2026-06-16.md`
  §3.3 L0-C).
- No new software thermal clamp; emergency protection stays hardware-owned.
- No change to the sidecar-gated actuation path (that is the separate L0-A1 lever).
- No priority elevation (separate, deferred L0-B lever).

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| No fan write / start / stop / breaker reset outside an explicit live task | `AGENTS.md` §Live Runtime Safety | Force-kill is a process-lifecycle action on the already-authorized watchdog/`--restart` path; it issues no fan write. Fans hold last PWM during the gap; the relaunched worker reasserts authority through the normal startup path. |
| Write-once crash recovery: never `{durable hardware override + no durable baseline record}` | `docs/WRITE_ORCHESTRATION.md`, `pending_writes.cpp`, `app_main.cpp` | A force-kill is equivalent to a crash; the existing `ReconcilePendingWrites` startup gate restores the captured baseline from `pending_writes.json`. No new actuation and no change to the Upsert-before-ApplyDuty ordering. |
| Single supervisor per runtime-home | `src/control/control_supervisor.cpp` (singleton mutex) | The escalation must terminate the existing worker/supervisor and confirm exit before relaunch, so the supervisor-singleton guarantee holds across the forced restart. |
| Runtime sidecar / status / event schema stays backward-compatible | `docs/RUNTIME_HOME.md` | Adds supervisor process-lifecycle event types (additive); no existing field, file, or schema version changes. |
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | Uses the Win32 process API already used by the supervisor; no new external dependency. |

## 5. Behavior specification

The change is scoped to the `--restart` handler and the supervisor stop/relaunch
path (`src/app/app_main.cpp:190-197`, `src/control/control_supervisor.cpp` —
`RequestStopAndWait` and the supervisor worker loop).

- On `--restart`, request the graceful stop as today. If it confirms worker (and
  supervisor) exit within the deadline, relaunch as today — unchanged path.
- If the graceful stop **times out** (returns `2`), escalate: resolve the
  worker PID (and supervisor PID) from the supervisor state / `control_runtime.json`
  and force-terminate them, confirm exit, then proceed to relaunch
  (`start_requested = true`) — instead of the current return-without-relaunch.
- The escalation must be bounded (a small, fixed number of force-terminate attempts
  with a short confirm wait) so it cannot itself hang the watchdog task (which has a
  2-minute `ExecutionTimeLimit`).
- A normal graceful stop, or a slow-but-live worker that honors the stop within the
  deadline, must take the unchanged path and must **not** be force-terminated.
- After relaunch, recovery is the existing startup path: `ReconcilePendingWrites`
  restores any orphaned baseline, then the loop reasserts authority and recaptures
  baselines.

## 6. Requirements  *(promotion gate 4 — assign IDs only after the design decision picks a direction)*

| ID | Requirement |
|---|---|
| REQ-WATCHDOG-01 | When `--restart`'s graceful stop times out (the hung-worker case), the controller must force-terminate the worker (and the supervisor if still present), confirm exit, and then relaunch — rather than returning without relaunch. |
| REQ-WATCHDOG-02 | The forced-termination escalation must emit a structured runtime event (e.g. `supervisor.worker_force_terminated`) recording the timed-out graceful stop, the terminated PID(s), and the reason, so the escalation is observable in the event log. |
| REQ-WATCHDOG-03 | The forced termination must preserve the write-once crash-recovery contract: a force-kill is treated as a crash, so the relaunched worker runs `ReconcilePendingWrites` and reasserts authority on startup, introducing no new `{durable hardware override + no durable baseline}` state. |
| REQ-WATCHDOG-04 | The escalation must trigger only on the stop-timeout (hung) condition; a normal graceful stop or a worker that honors the stop within the deadline must not be force-terminated, and the escalation must be bounded so it cannot hang the watchdog task. |

IDs come from this feature's `REQ-WATCHDOG-*` namespace, reserved in the registry in
`README.md`. Keep them stable once published.

## 7. Data / schema deltas

- New/changed fields: additive event types
  `supervisor.worker_force_terminated`,
  `supervisor.supervisor_force_terminated`, and
  `supervisor.force_terminate_failed` (PID when available and reason; worker
  force-terminate success records the prior graceful-stop result). No status,
  sidecar, manifest, CSV, or config field changes.
- Config impact (`config/control.*.json`, `config/machines/*.json`): none for v1
  (the grace period reuses the existing `RequestStopAndWait` deadline). A
  configurable force-kill grace period is an open decision (§11).
- Schema/version impact: none — additive event only; no version bump. No existing
  runtime-home file, archive, or config becomes invalid.

## 8. CLI / config / operator surface deltas

No new CLI subcommand or flag. The escalation is internal to the existing
`--restart` path the watchdog already invokes. The only operator-visible change is
the new event-log entries on a forced recovery.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| `docs/watchdog-hung-worker-recovery-decision-2026-06-16.md` | Force-kill escalation is the chosen recovery: D1 worker-first (supervisor self-exits), D2 reuse the 15 s graceful deadline, D3 bounded single-shot with a PID-reuse image-path guard. Basis: `docs/cpu-loop-survival-layer0-plan-2026-06-16.md` §3.2 and `docs/cpu-loop-stall-reproduction-findings-2026-06-16.md`. | Current (implemented) |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-WATCHDOG-01 | T, M | Integration test drives a worker that ignores the stop sentinel past the deadline and asserts force-terminate + relaunch; live hung-worker repro (`docs/cpu-loop-stall-reproduction-protocol-2026-06-15.md` A4) shows recovery. |
| REQ-WATCHDOG-02 | T, R | Test asserts the `supervisor.worker_force_terminated` event is emitted with PID/reason; review against the event schema in `docs/RUNTIME_HOME.md`. |
| REQ-WATCHDOG-03 | T, R | Reconcile-after-force-kill test (orphaned sidecar restored on relaunch); review vs `docs/WRITE_ORCHESTRATION.md` that ordering/recovery is unchanged. |
| REQ-WATCHDOG-04 | T, R | Test that a worker honoring the graceful stop is not force-terminated and that the escalation is attempt-bounded; review of the trigger gate. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

D1–D3 are resolved in `docs/watchdog-hung-worker-recovery-decision-2026-06-16.md`.
Remaining items are non-blocking and post-v1:

| Decision | Needed before | Current default |
|---|---|---|
| Configurable force-kill grace period vs. the fixed 15 s deadline | post-v1 tuning | Fixed 15 s (reuse the existing `WaitForRuntimeStop` deadline). |
| Apply the same force-stop escalation to a plain `--stop` after timeout | post-v1 | Graceful-only for `--stop`; escalation scoped to `--restart` recovery. |

**Known limitations (v1):**
- *PID-corroboration refusal on a pre-first-write freeze.* The worker-PID guard refuses
  to force-terminate when `control_supervisor.json` `last_worker_pid` disagrees with
  `control_runtime.json` `process_id`. If a worker freezes *before* its first status
  write while a prior incarnation's `process_id` still sits in `control_runtime.json`,
  the PIDs disagree and the escalation refuses; because the frozen worker can never
  republish its PID, every subsequent watchdog poll refuses too, so this narrow startup
  race is not auto-recovered until external intervention. This is the fail-closed
  direction by design (an uncorroborated PID is never terminated) and is not a
  regression from the pre-FEAT-0008 detect-but-don't-recover behavior, but it is a case
  this recovery path does not clear. A future option is to fall back to the supervisor
  sidecar PID (or re-resolve the worker by parent/handle) when corroboration is absent.
- *Image-path guard assumes a non-reparse-point install.* The guard compares
  `GetModuleFileNameW` (the `--restart` invoker) against `QueryFullProcessImageNameW`
  (the live target) with `_wcsicmp` and no canonicalization. Verified to match on the
  current deployment (installer resolves to `.ProviderPath` at task registration;
  `release\` is not a junction/symlink). A `subst`/mapped-drive/junctioned `release\`
  could make the same file render as two strings, in which case the guard refuses
  (fail-closed, deferred recovery) rather than killing the wrong process.

## 12. Measurement gate & dependencies

- **Measurement gate:** does not cross `docs/MEASUREMENT_GATE.md` — no cadence,
  channel-count, or mixed-input-strategy change; this is a process-recovery path.
- **Depends on:** the existing watchdog task and supervisor/`RequestStopAndWait`
  path; the write-once recovery (`ReconcilePendingWrites`).
- **Build/test impact:** one new C++ orchestration unit test target and one
  Python integration test module for the hung-worker path; no
  `CONTROL_PIPELINE_MATH` change.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [x] 3. Required design decision record(s) written and marked current (§9).
- [x] 4. Concrete `REQ-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — `Test-LocalCI`, build-release, contract review, or runtime evidence (§10), and mirrored in `docs/TRACEABILITY.md`.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline.
- [x] 7. Doctrine check: claims are grounded; `must`/`should`/`is` used per `CLAUDE.md`; no undefined terms or unqualified vague adjectives.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-WATCHDOG-01 | pass (T, M) | (T) `tests/test_watchdog_force_terminate.py::test_hung_worker_is_force_terminated_and_relaunched`: the worker is suspended (`NtSuspendProcess`) so it cannot honor the stop sentinel, `--restart` force-terminates it, and the relaunched worker PID **differs** from the killed one. (M) live deploy 2026-06-16 (commit `e5bafdb`): the live control-loop worker (pid 44984) was suspended; the production watchdog scheduled task fired `--restart`, the 15 s graceful stop timed out, the worker was force-terminated (`supervisor.worker_force_terminated`, detail "stop_result=2"), and a fresh worker (pid 36348) relaunched and resumed ticking. **Scope:** (T) and (M) both verify the recovery *mechanism* using an `NtSuspendProcess` hung-worker proxy (the deterministic way to make a worker miss the 15 s stop). Whether a worker freezes that hard *naturally* is a separate open Layer-0 characterization question — observed load only degrades-and-graceful-recovers (`docs/cpu-loop-survival-live-sweep-findings-2026-06-16.md`; protocol §5 AVX-512 escalation not run) — and is not a REQ-WATCHDOG-01 recovery-path gap. | 2026-06-16 |
| REQ-WATCHDOG-02 | pass | Same integration test asserts the `supervisor.worker_force_terminated` event records the killed PID; `docs/RUNTIME_HOME.md` documents the three additive `supervisor.*force_terminate*` event types (R). | 2026-06-16 |
| REQ-WATCHDOG-03 | pass | Same integration test seeds an orphaned `pending_writes.json` entry that the force-killed relaunch reconciles to `[]`; the escalation does not touch the reconcile path (`app_main.cpp` startup `ReconcilePendingWrites` unchanged; a force-kill is a crash) (R). | 2026-06-16 |
| REQ-WATCHDOG-04 | pass | `tests/test_watchdog_force_terminate.py::test_graceful_worker_is_not_force_terminated` (a graceful stop never escalates) + `tests/cpp/worker_force_terminate_tests.cpp` (image-guard refusal, PID-corroboration refusal, single-shot no-retry bound); trigger gated on `stop_result == 2` in `app_main.cpp` (R). (M) the 2026-06-16 live AVX2 starvation sweep (`cpu-synth-load` high / pin / oversubscribe×4, sweep-wide peak `cpu_tctl_c` 76.4 °C) degraded loop cadence but the worker honored the graceful stop each time: two watchdog restarts, both `supervisor.worker_exited exit_code=0 stop_requested=true`, **zero** force-terminations — force-terminate is correctly gated to the hard-freeze case and the AVX2 sweep did not reproduce that class (protocol §5 would need an AVX-512 power-matched load, not run). | 2026-06-16 |

**Spec vs. implementation deltas:**
- **Testable seam (additive, no behavior change).** The escalation logic lives
  in `src/control/worker_force_terminate.{h,cpp}` rather than inside
  `control_supervisor.cpp` as §9's plan suggested, behind an injectable
  `ProcessTerminator` interface (the same DI idiom as `FanWriter` /
  `RecordingFanWriter`). The pure orchestration — worker-first ordering, the
  PID-reuse image guard, the worker-PID corroboration guard, the single-shot
  bound, and the `runtime_gone` relaunch gate — is unit-tested with a fake
  (`tests/cpp/worker_force_terminate_tests.cpp`); the real `Win32ProcessTerminator`
  opens **one** handle per PID and uses it for the image-path guard,
  `TerminateProcess`, and the post-kill exit confirm, so a confirmed-gone result
  means death verified on the held handle (not a racy re-`OpenProcess` that PID
  reuse could defeat).
- **PID-corroboration guard.** D3's image-path guard is implemented as a full
  case-insensitive image-path match against the `--restart` invoker's own
  executable, plus a corroboration check: the supervisor sidecar's
  `last_worker_pid` is cross-checked against `control_runtime.json`'s
  `process_id`, and a disagreement refuses the kill (an uncorroborated PID is
  never terminated).
- **Integration-test fixture.** REQ-WATCHDOG-01's (T) creates a genuinely frozen
  worker by suspending the real process (`NtSuspendProcess` via `ctypes`),
  rather than adding a test-only "ignore-stop" mode to the controller as §4
  contemplated — no production worker code path was added.
- **No change** to `RequestStopAndWait`, the worker, the tick loop, the sidecar
  gate, or the supervisor crash-restart loop; the `--restart` handler gains a
  single `stop_result == 2` branch that calls the escalation and relaunches only
  when the runtime is confirmed gone.
