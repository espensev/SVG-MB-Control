# FEAT-0008: Watchdog hung-worker recovery (force-kill escalation)

**Project:** svg-mb-control
**Status:** Draft   **Version:** 0.1   **Updated:** 2026-06-16
**Namespace:** `REQ-WATCHDOG-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/WRITE_ORCHESTRATION.md`
**Purpose:** when the watchdog detects a stale (hung) worker but the graceful
stop times out, escalate to a forced termination and relaunch so a truly frozen
control loop is actually recovered, not merely detected.

## 1. Summary

Today the watchdog *detects* a hung worker (health exit code 2, `kStale`) and
issues `--restart`, but if the worker is frozen hard enough not to honor the
graceful stop within `RequestStopAndWait`'s deadline, the controller returns
**without relaunching** and no force-kill exists, so the loop stays down until the
next event (reboot, manual restart). This feature adds a bounded force-terminate
escalation on the restart path: after the graceful stop times out, force-terminate
the worker (and supervisor if still present), then relaunch — recovering the
`{worker alive but frozen}` case that the current path detects but cannot clear.

## 2. Problem & motivation  *(promotion gate 1)*

Verified on 2026-06-16 from source: on `--restart` the handler calls
`RequestStopAndWait` (`src/control/control_supervisor.cpp:399-423`, 15 s deadline)
and then `app_main.cpp:190-197` does `if (stop_result != 0) return stop_result;`
**before** `options.start_requested = true`. So a stop that times out aborts the
relaunch. A grep of `src/` finds **no** `TerminateProcess` / force-kill in the
watchdog or supervisor paths. The watchdog scheduled task therefore detects a
hung worker (`kStale` → exit 2 → `--restart`) but cannot recover one that will not
honor the stop sentinel.

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
| Runtime sidecar / status / event schema stays backward-compatible | `docs/RUNTIME_HOME.md` | Adds one new event type (additive); no existing field, file, or schema version changes. |
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | Uses the Win32 process API already used by the supervisor; no new external dependency. |

## 5. Behavior specification

The change is scoped to the `--restart` handler and the supervisor stop/relaunch
path (`src/app/app_main.cpp:190-197`, `src/control/control_supervisor.cpp` —
`RequestStopAndWait` and the supervisor worker loop).

- On `--restart`, request the graceful stop as today. If it confirms worker (and
  supervisor) exit within the deadline, relaunch as today — unchanged path.
- If the graceful stop **times out** (returns non-zero), escalate: resolve the
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

- New/changed fields: one new event type `supervisor.worker_force_terminated`
  (additive event-log entry: PID(s), prior graceful-stop result, reason). No
  status, sidecar, manifest, CSV, or config field changes.
- Config impact (`config/control.*.json`, `config/machines/*.json`): none for v1
  (the grace period reuses the existing `RequestStopAndWait` deadline). A
  configurable force-kill grace period is an open decision (§11).
- Schema/version impact: none — additive event only; no version bump. No existing
  runtime-home file, archive, or config becomes invalid.

## 8. CLI / config / operator surface deltas

No new CLI subcommand or flag. The escalation is internal to the existing
`--restart` path the watchdog already invokes. The only operator-visible change is
the new event-log entry on a forced recovery. Update `docs/STRUCTURE_AND_STABILITY.md`
and the watchdog/runtime docs at implementation if the restart path's documented
behavior changes (`AGENTS.md` §Change Checklist).

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| `docs/cpu-loop-survival-layer0-plan-2026-06-16.md` (§3.2 L0-A4) | That force-kill escalation is the chosen recovery for a hung worker (vs. leaving detection-only). | Proposed |
| `docs/watchdog-hung-worker-recovery-decision-YYYY-MM-DD.md` (to write) | Worker-only vs. worker+supervisor termination; grace period source (reuse 15 s vs. config); force-kill attempt bound. | Pending (gate 3) |

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

| Decision | Needed before | Current default |
|---|---|---|
| Force-terminate worker only, or worker + supervisor when both are wedged? | implementation | Worker first; supervisor only if it also fails to exit. |
| Grace period before force-kill: reuse the 15 s `RequestStopAndWait` deadline or a separate config key? | implementation | Reuse the existing deadline for v1. |
| Bound on force-kill escalations (avoid a force-kill/relaunch loop on a persistently wedged machine)? | implementation | Cap attempts, then fall back to the supervisor backoff and log. |

## 12. Measurement gate & dependencies

- **Measurement gate:** does not cross `docs/MEASUREMENT_GATE.md` — no cadence,
  channel-count, or mixed-input-strategy change; this is a process-recovery path.
- **Depends on:** the existing watchdog task and supervisor/`RequestStopAndWait`
  path; the write-once recovery (`ReconcilePendingWrites`).
- **Build/test impact:** one new integration test for the hung-worker path; no
  `CONTROL_PIPELINE_MATH` change.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [ ] 3. Required design decision record(s) written and marked current (§9).
- [x] 4. Concrete `REQ-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — `Test-LocalCI`, build-release, contract review, or runtime evidence (§10), and mirrored in `docs/TRACEABILITY.md`.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline.
- [x] 7. Doctrine check: claims are grounded; `must`/`should`/`is` used per `CLAUDE.md`; no undefined terms or unqualified vague adjectives.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-WATCHDOG-01 | | | |
| REQ-WATCHDOG-02 | | | |
| REQ-WATCHDOG-03 | | | |
| REQ-WATCHDOG-04 | | | |

**Spec vs. implementation deltas:** <record anything built differently from this
spec, and why. If behavior changed, update §5/§6, refresh the cited contract docs
per `AGENTS.md` §Change Checklist, and bump **Updated**.>
