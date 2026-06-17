# FEAT-0009: Controller scheduling-priority elevation

**Project:** svg-mb-control
**Status:** Draft (held — promotion gates open pending the §12 A/B contention experiment)   **Version:** 0.1   **Updated:** 2026-06-17
**Namespace:** `REQ-PRIORITY-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/CONTROL_LOOP.md`
**Purpose:** propose raising the control worker's Windows scheduling priority — and
co-elevating its recovery actors — to reduce control-loop cadence degradation under
heavy CPU contention, gated on an experiment that must first show the degradation is
scheduling-bound.

## 1. Summary

This feature proposes elevating the control worker process from its current
**BelowNormal** scheduling priority to a configured higher level — the aggressive
opt-in level being `HIGH_PRIORITY_CLASS` with the single control-tick thread at
`THREAD_PRIORITY_TIME_CRITICAL` (effective priority 15) — selectable and reversible
through a new `process_priority` config key whose **absent-key default is `inherit`**
(no elevation unless a deployment opts in). To keep the FEAT-0008 force-terminate
recovery path from being outranked by an elevated worker, the supervisor process and
the process that actually performs the force-terminate (the `svg-mb-control.exe
--restart` child) are co-elevated; raising the watchdog scheduled-task `<Priority>`
alone is insufficient because it does not propagate to that child (§5). The proposed
operator-visible outcome is a lower control-loop cadence-degradation rate under
sustained high-priority CPU load. The feature is held at Draft until an A/B
experiment (§12) shows the degradation is scheduling-bound rather than file-lock
bound; priority does not shorten a file-lock wait.

## 2. Problem & motivation  *(promotion gate 1)*

The control worker currently runs at **BelowNormal priority** — and not by design:
both `CreateProcessW` launch sites
(`src/platform/task_runner.cpp:133`, `src/control/control_supervisor.cpp:348`) pass
**no** priority-class creation flag, and no install script sets a scheduled-task
`<Priority>`, so the worker inherits the Windows Task Scheduler default (priority 7,
the BelowNormal region). Under the reproduction protocol
(`docs/cpu-loop-stall-reproduction-protocol-2026-06-15.md`), `above`-priority
synthetic load (`cpu-synth-load --priority above`) coincided with a control-loop
stall in **1 of 3** runs and degraded the tick cadence (~1 tick/s) in the others
(`docs/cpu-loop-stall-reproduction-findings-2026-06-16.md`); the controller is
outranked by such load.

**The motivation is bounded by an unresolved causal question, stated here
honestly:** the Phase-1 analysis attributes that stall to a *coincidence of a
file-lock (sidecar `Upsert` error 5) and the 10 s staleness recycle*
(`docs/cpu-loop-survival-layer0-plan-2026-06-16.md` §1, §3.1) — **not solely** to
controller CPU starvation: the proximate trigger is the file-lock + staleness
coincidence, of which sustained CPU starvation is one contributing leg (plan §1
leg b). "The load was high-priority" does not establish "the stall was a
scheduling stall." Raising the controller's priority cannot shorten a file-lock
wait. Therefore the observed degradation is real and evidence-sourced, but the
evidence that *this lever* addresses it does **not yet exist**. This spec exists to
gate the experiment that would obtain that evidence (§12), and stays held-Draft
until it does — the same posture as `FEAT-0003` (design captured, not promoted).

## 3. Goals & non-goals

**Goals**
- Provide a configured, reversible mechanism to raise the control worker's
  scheduling priority above competing user-mode load.
- Keep the FEAT-0008 recovery path (force-terminate + relaunch) able to preempt and
  recover an elevated, misbehaving worker.
- Define the A/B experiment whose result determines whether the lever ships, stays
  held, or is rejected.

**Non-goals**
- The file-lock / sidecar-gate write-path stall (that is lever **L0-A1**, the plan's
  primary write-path fix, and the prerequisite the plan ranks ahead of this one).
- `REALTIME_PRIORITY_CLASS` or any thread priority above 15 (forbidden by the
  Layer-0 invariants, `docs/cpu-loop-survival-layer0-plan-2026-06-16.md` §7).
- CPU affinity / pinning, processor-group placement, or I/O-priority tuning.
- Any change to control-loop cadence, channel set, or control math.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| No fan write / start / stop / breaker reset outside an explicit live task | `AGENTS.md` §Live Runtime Safety | Priority is a process/thread scheduling attribute applied once at worker startup; it issues no actuation. The default is reversible from config plus a worker relaunch with no rebuild (REQ-PRIORITY-02). |
| Only-raise-never-lower priority; **no `REALTIME_PRIORITY_CLASS`** | `docs/cpu-loop-survival-layer0-plan-2026-06-16.md` §7 | The mechanism only raises from the inherited BelowNormal baseline and caps at `TIME_CRITICAL` (15); `inherit` is a no-op; `REALTIME` is never selectable (REQ-PRIORITY-01). |
| Fail-safe floor: fans hold last PWM on stall; SMU 95 °C + SuperIO/EC backstop unaffected | `docs/cpu-loop-survival-layer0-plan-2026-06-16.md` §7 | Priority does not touch the write gate or the hardware backstops; a skipped fan write remains the existing safe state. |
| Watchdog recovery of a hung worker must still preempt the worker | `docs/features/FEAT-0008-watchdog-hung-worker-recovery.md` | The supervisor **and the `svg-mb-control.exe --restart` process that executes the force-terminate** are elevated so the kill path is not outranked by a worker at 15; a raised scheduled-task `<Priority>` alone does not reach that child (REQ-PRIORITY-04). |
| Shipped 250 ms cadence / channel set is the measured baseline | `docs/MEASUREMENT_GATE.md` | No cadence, channel-count, or mixed-input change; priority does not move the measurement baseline (§12). |
| Runtime sidecar / status / manifest schema stays backward-compatible | `docs/RUNTIME_HOME.md` | Only an additive optional config key and an additive startup event; no existing runtime-home file, archive, or config becomes invalid (§7). |

## 5. Behavior specification

**Proposed behavior (not yet implemented).** At worker startup the process reads the
`process_priority` config key and applies the selected level once, before the tick
loop begins:

- `high_timecritical` (the aggressive opt-in level): `SetPriorityClass(HIGH_PRIORITY_CLASS)` on
  the worker process, then `SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL)` on the
  control-tick thread (the control worker spawns no app worker threads — no
  `std::thread`/`CreateThread`/`_beginthreadex` in first-party `src/` — so the tick
  loop runs on the process main thread). Effective scheduling priority 15.
- `above_normal`: `ABOVE_NORMAL_PRIORITY_CLASS`, no per-thread bump.
- `normal`: `NORMAL_PRIORITY_CLASS`, no per-thread bump.
- `inherit`: no priority call (retains the inherited Task Scheduler default).

The control-tick body (`src/control/tick_runner.cpp`) is unchanged. Between ticks the
thread blocks in a kernel timed wait — `std::condition_variable::wait_until`
(`src/control/control_scheduler.cpp`, `WaitForNextControlTick`) — waking only every
50 ms to stat the stop-request file, and the sidecar retry uses exponential-backoff
`std::this_thread::sleep_for` (`src/runtime/json_io.cpp`); so the elevated thread is
not CPU-runnable between ticks beyond that ~20 Hz stop-file check (REQ-PRIORITY-03).
Applying the level is the only new behavior on the worker control path.

**Recovery co-elevation (R2).** Because `TIME_CRITICAL` (15) is the top of the
non-REALTIME range and `REALTIME_PRIORITY_CLASS` is forbidden, the recovery actors
cannot be placed *strictly above* the worker; they are co-elevated to the **same** top
level so the kill path is co-scheduled rather than outranked. **Two processes must be
elevated, not one:**
- the **supervisor process** to `HIGH_PRIORITY_CLASS`+`TIME_CRITICAL` (in-process
  `SetPriorityClass`/`SetThreadPriority`); it mostly blocks on `WaitForSingleObject`
  (`src/control/control_supervisor.cpp`), so equal-top adds negligible load while
  keeping it immediately schedulable when the worker dies.
- the **process that actually performs the force-terminate** — the
  `svg-mb-control.exe --restart` child that the watchdog task launches via
  `CreateProcessW` (`src/platform/task_runner.cpp`) and that runs `EscalateForceTerminate`
  (`src/control/worker_force_terminate.{h,cpp}`). This child must be given
  `HIGH_PRIORITY_CLASS` explicitly (a creation flag at that `CreateProcessW` site, or a
  self-`SetPriorityClass` at `--restart` startup). **Raising the watchdog scheduled-task
  `<Priority>` alone is insufficient:** that `<Priority>` sets the *task-runner* process
  class, but `CreateProcessW` does not propagate a `HIGH` parent class to a no-flag
  child, so the `--restart` grandchild would otherwise default to `NORMAL` (base 8) and
  remain outranked by a worker at 15.

The FEAT-0008 force-terminate sequence itself is otherwise unchanged.

**Failure / edge behavior.** A failed `SetPriorityClass`/`SetThreadPriority` is
logged and the worker continues at its inherited priority (degraded, not fatal); the
loop never blocks on the priority call. An unrecognized `process_priority` value is
treated as `inherit` and logged.

## 6. Requirements  *(promotion gate 4 — assign IDs only after the design decision picks a direction)*

| ID | Requirement |
|---|---|
| REQ-PRIORITY-01 | When `process_priority = high_timecritical`, the worker applies `HIGH_PRIORITY_CLASS` to the process and `THREAD_PRIORITY_TIME_CRITICAL` to the control-tick thread once at startup (effective priority 15); the mechanism only raises from the inherited baseline and never selects `REALTIME_PRIORITY_CLASS`. |
| REQ-PRIORITY-02 | A `process_priority` config key (`high_timecritical` \| `above_normal` \| `normal` \| `inherit`) selects the level, and **an absent key resolves to `inherit`** (no elevation on a config that omits it); a non-aggressive value disables the elevation and takes effect on the next worker relaunch with no rebuild (kill-switch). The resolved level is emitted once at startup via the additive `supervisor.priority_applied` event. |
| REQ-PRIORITY-03 | The elevated control-tick thread does not busy-wait: between ticks it is a kernel sleep and the sidecar retry uses backoff sleeps, so the worker consumes near-zero CPU between ticks even at priority 15. |
| REQ-PRIORITY-04 | The supervisor process **and the `svg-mb-control.exe --restart` process that executes the force-terminate** are elevated so the FEAT-0008 recovery path is not outranked by a worker at priority 15, and that path still terminates and relaunches an elevated worker. A raised watchdog scheduled-task `<Priority>` alone does not satisfy this, because `CreateProcessW` does not propagate a `HIGH` class to the no-flag `--restart` child (§5). |
| REQ-PRIORITY-05 | Elevation does not lengthen contention on the system-wide `Global\Access_PCI` / PawnIO path: the elevated thread holds no spin while blocked on that mutex or on file I/O, and its wait on that mutex is bounded (`kPciMutexTimeoutMs`, after which the read degrades to a warning snapshot and fans hold last PWM). The §12 experiment observes no system-wide responsiveness regression. |
| REQ-PRIORITY-06 | Promotion past Draft requires the §12 A/B experiment to show a measurable, scheduling-attributable cadence-degradation reduction under `above`-priority load; absent that evidence the spec stays held-Draft (the `FEAT-0003` pattern). |

IDs come from this feature's `REQ-PRIORITY-*` namespace, reserved in the registry in
`README.md`. Keep them stable once published.

## 7. Data / schema deltas

- New/changed fields: `process_priority` — string enum
  (`high_timecritical` \| `above_normal` \| `normal` \| `inherit`), optional. A
  proposed additive startup event (`supervisor.priority_applied`, recording the
  resolved level) gives operator-visible confirmation; no status/sidecar/manifest
  field changes.
- Config impact (`config/control.*.json`, `config/machines/*.json`): one new optional
  key, `process_priority`. **Upgrade note:** the absent-key default is `inherit`, so a
  deployment whose config omits the key is **not** elevated on upgrade; elevation is an
  explicit opt-in. The §12 experiment may later justify shipping an aggressive default,
  but until it does, no box is silently elevated.
- Schema/version impact: none beyond the additive optional key and additive event at
  implementation; no existing runtime-home file, archive, or config becomes invalid.
  The `docs/RUNTIME_HOME.md` event list is updated at implementation per `AGENTS.md`
  §Documentation Maintenance.

## 8. CLI / config / operator surface deltas

- **Config:** the `process_priority` key (§7). No new CLI subcommand or flag.
- **Operator/install:** the watchdog scheduled-task `<Priority>` in
  `Install-SVG-MB-ControlWatchdogScheduledTask.ps1` changes at implementation
  (REQ-PRIORITY-04), visible at task registration.
- **Observability:** the proposed `supervisor.priority_applied` startup event records
  the resolved level. `README.md` and `docs/CONTROL_LOOP.md` are updated when the
  surface lands (`AGENTS.md` §Change Checklist).

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| `docs/controller-priority-elevation-decision-2026-06-17.md` | The aggressive opt-in level (`HIGH`+`TIME_CRITICAL` = 15), the config surface with an `inherit` absent-key default + kill-switch, recovery co-elevation of the supervisor **and the `--restart` killer process** (D-PRIO-3; a raised task `<Priority>` does not reach the killer), and the held-Draft-pending-experiment posture (D-PRIO-4). | Current |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-PRIORITY-01 | T, R | Unit test of a `ProcessPriority` seam (the `FanWriter`/`ProcessTerminator` DI idiom): asserts the level→class/thread-priority mapping, raise-only, and that `REALTIME` is unreachable; code review of the startup apply site in `app_main.cpp`. |
| REQ-PRIORITY-02 | T, R | Config-parse test of the `process_priority` enum + default; review that a non-aggressive value disables the elevation and that it is read at startup (relaunch-applied), not requiring a rebuild. |
| REQ-PRIORITY-03 | M, R | (R) review that every inter-tick / retry wait on the elevated thread is a kernel wait — `wait_until` (`control_scheduler.cpp`) + backoff `sleep_for` (`json_io.cpp`), no busy-spin (a negative not provable by unit test); (M) **hard promotion blocker** — measure worker CPU% near zero between ticks while elevated on the live box. |
| REQ-PRIORITY-04 | T, M, R | Recovery-against-elevated-worker test (FEAT-0008 force-terminate succeeds with the worker at 15); (M) live force-terminate of an elevated suspended worker; (R) review that the `--restart` killer process and the supervisor are elevated (a raised task `<Priority>` does not propagate to the killer). |
| REQ-PRIORITY-05 | M, R | (M) the §12 experiment records no system-wide responsiveness regression and no `Global\Access_PCI`-attributed stall increase; review that no spin is held across the mutex/file-I/O wait. |
| REQ-PRIORITY-06 | M | (M) the §12 A/B experiment result; promotion is blocked until it shows a scheduling-attributable degradation reduction. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Whether `above_normal` alone (process class 13, no thread-15) suffices vs the full `high_timecritical` | the §12 experiment | Offer both; experiment compares. |
| Whether the level is hot-reloadable vs startup-only | implementation | Startup-only (applied once); hot-reload deferred. |

## 12. Measurement gate & dependencies

- **Measurement gate:** does **not** cross `docs/MEASUREMENT_GATE.md` (no faster
  cadence, no added live channels, no broader mixed-input strategy). The
  characterization evidence required before promotion is the **A/B contention
  experiment**: launch the live controller via an *external wrapper* at the candidate
  priority (no product code), re-run the reproduction protocol repeatedly under
  `above`-priority load, and compare cadence-degradation / stall rate on vs off, plus
  a system-wide responsiveness check (REQ-PRIORITY-05/06). Because the stall is
  probabilistic, success = degradation rate reduced across many runs, not a single
  pass.
- **Depends on:** `FEAT-0008` (the recovery path it co-elevates and must not
  regress). The Layer-0 plan ranks **L0-A1** (sidecar-upsert retry) ahead of this
  lever; this spec defers to that ordering by remaining held-Draft until the
  experiment justifies it.
- **Build/test impact:** a `ProcessPriority` seam + unit test; a
  recovery-against-elevated-worker test; the watchdog task XML `<Priority>` change;
  the external experiment wrapper (a script, not product code). No
  `CONTROL_PIPELINE_MATH` change.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [ ] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2). *(Degradation observed, but scheduling-causation / lever efficacy is not yet evidenced — blocked on the §12 A/B experiment. This is the held gate.)*
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [x] 3. Required design decision record(s) written and marked current (§9).
- [x] 4. Concrete `REQ-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — `Test-LocalCI`, build-release, contract review, or runtime evidence (§10), and mirrored in `docs/TRACEABILITY.md`.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline.
- [x] 7. Doctrine check: claims are grounded; `must`/`should`/`is` used per `CLAUDE.md`; no undefined terms or unqualified vague adjectives.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

Not started — the feature is held at Draft. Each row is filled after implementation,
which is not authorized until the §12 experiment justifies promotion.

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-PRIORITY-01 | | | |
| REQ-PRIORITY-02 | | | |
| REQ-PRIORITY-03 | | | |
| REQ-PRIORITY-04 | | | |
| REQ-PRIORITY-05 | | | |
| REQ-PRIORITY-06 | | | |

**Spec vs. implementation deltas:** none yet (not implemented).
