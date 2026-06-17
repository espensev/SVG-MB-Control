# Controller Scheduling-Priority Elevation — Decision & Plan — 2026-06-17

**Status:** Current — decision settled 2026-06-17. Settles the direction for FEAT-0009
promotion gate 3. The feature itself is **held at Draft**: nothing here authorizes
code. The decision records *which* priority design FEAT-0009 commits to **if** the §12
experiment justifies promotion.
**Owns:** `docs/features/FEAT-0009-controller-priority-elevation.md`
(`REQ-PRIORITY-*`).
**Basis:** `docs/cpu-loop-survival-layer0-plan-2026-06-16.md` §3.3 (L0-B, the
deferred controller-priority lever and its invariants §7); the scheduling-axis
evidence in `docs/cpu-loop-stall-reproduction-findings-2026-06-16.md` (1/3 stall +
cadence degradation under `above`-priority load); and the
`Global\Access_PCI`/whole-system-freeze mechanism in
`docs/cpu-0609-freeze-classification-2026-06-16.md` §1, which bounds the inversion
risk below.
**Scope guard:** this record captures the decision and a held-Draft design. Building
it still requires the §12 contention experiment *and* the `AGENTS.md` Feature Intake
Gate authorization.

## 1. Context (the gap, and the honest limit on it)

Verified from source 2026-06-17: the control worker runs at **BelowNormal** priority
by inheritance, not design. Neither `CreateProcessW` site
(`src/platform/task_runner.cpp:133`, `src/control/control_supervisor.cpp:348`) sets a
priority-class flag, and no install script sets a scheduled-task `<Priority>`, so the
worker takes the Windows Task Scheduler default (priority 7). Under `above`-priority
synthetic load the controller is outranked: 1/3 runs stalled, the rest degraded to
~1 tick/s (`cpu-loop-stall-reproduction-findings-2026-06-16.md`).

**The load-bearing caveat:** the Layer-0 plan attributes that stall to a *file-lock +
10 s-staleness coincidence* — not *solely* controller CPU starvation, which is one
contributing leg of that coincidence
(`cpu-loop-survival-layer0-plan-2026-06-16.md` §1, §3.1). Priority cannot shorten a
file-lock wait. So the evidence that *raising priority* addresses the observed
degradation does not yet exist. This decision is therefore explicitly conditional: it
fixes the design so the experiment that would settle it can run, and defers building
to that result.

## 2. Decisions

### D-PRIO-1 — Aggressive level: HIGH class + TIME_CRITICAL tick thread (priority 15)

Apply `HIGH_PRIORITY_CLASS` to the worker process and `THREAD_PRIORITY_TIME_CRITICAL`
to the single control-tick thread (the control worker spawns no app worker threads on
the control path), giving effective priority 15. Applied once at startup.

*Rationale:* the operator chose the brief's original aggressive target. 15 is the top
of the non-REALTIME range; it strongly resists preemption by `above`/`high` user
load. Rejected: `REALTIME_PRIORITY_CLASS` (16–31) — forbidden by the Layer-0
invariant (§7) and unnecessary, since the loop is a 250 ms cadence that sleeps
between ticks, not a real-time deadline task.

### D-PRIO-2 — Config-gated, `inherit` absent-key default, aggressive opt-in, kill-switch

A `process_priority` key (`high_timecritical | above_normal | normal | inherit`)
selects the level. The **absent-key default is `inherit`** — a deployment that omits
the key is not elevated — and `high_timecritical` is the explicit aggressive opt-in. A
non-aggressive value backs the elevation off and takes effect on the next worker
relaunch with no rebuild.

*Rationale:* this changes a live shipped process's system-wide scheduling weight, so
`AGENTS.md` Live Runtime Safety wants reversibility without a rebuild **and** no silent
elevation on upgrade. An `inherit` default is the upgrade-safe choice and is the only
one consistent with the rest of this decision: the feature is held-Draft because the
leading hypothesis is the stall is file-lock-bound (priority may buy nothing) and R1
flags an elevation-driven whole-system risk — so the safe default is to elevate only
where an operator opts in, with the §12 experiment the gate that could later justify a
different shipped default. (An earlier draft proposed default-aggressive-ON; that is
**rejected here** as inconsistent with the held-Draft / unproven-benefit posture and
with R1.)

### D-PRIO-3 — Co-elevate the actual recovery actors to equal-top (not strictly above)

A worker at 15 outranks its own FEAT-0008 killer (a priority-7 watchdog task, a
BelowNormal supervisor), so an elevated misbehaving worker could starve the recovery
path it depends on. The recovery actors are therefore co-elevated to the **same** top
level (because `REALTIME_PRIORITY_CLASS` is forbidden they cannot sit *strictly above*
15). **Two processes must be elevated, and identifying them correctly matters:**

1. the **supervisor** → `HIGH`+`TIME_CRITICAL` via in-process `SetPriorityClass`; it
   blocks on `WaitForSingleObject`, so equal-top costs negligible load.
2. the **`svg-mb-control.exe --restart` process that actually calls `TerminateProcess`**
   (`EscalateForceTerminate`). The watchdog scheduled task launches
   `task-runner.exe --watchdog-run`, which spawns this `--restart` child via
   `CreateProcessW` with no priority flag. **Raising only the scheduled-task
   `<Priority>` does not reach this child:** `CreateProcessW` does not propagate a
   `HIGH` parent class to a no-flag child, so the killer would default to `NORMAL`
   (base 8) and stay outranked by the worker at 15. The `--restart` child must
   therefore be elevated directly — a `HIGH_PRIORITY_CLASS` creation flag at the
   `task_runner.cpp` `CreateProcessW` site, or a self-`SetPriorityClass` at `--restart`
   startup.

*Rationale:* preserves the validated FEAT-0008 recovery by construction. This is a
**necessary correction to the original brief**, which named only the supervisor and the
watchdog task: co-elevating the task without elevating the `--restart` grandchild would
leave the killer at base 8 and *introduce* the very inversion REQ-PRIORITY-04 forbids.
The "equal-top round-robins, never starved" property holds only once the killer process
itself reaches the worker's level — and even then it is a scheduling guarantee for the
controller/recovery pair, not for the whole-system freeze class (R2). Rejected: relying
on a no-spin guarantee alone — it does not help if a future change introduces a spin;
a no-spin invariant is still required separately (see Risks R1).

### D-PRIO-4 — Held-Draft until the §12 experiment; the experiment uses no product code

FEAT-0009 stays at Draft (gate 1 open) until an A/B experiment shows the degradation
is scheduling-bound. The experiment launches the live controller via an **external
wrapper** at the candidate priority — no product code — and compares cadence-degradation
/ stall rate on vs off under `above`-load, with a system-wide responsiveness check.

*Rationale:* honors the plan's "revisit only with measured contention evidence"
precondition and its ordering (L0-A1 first). A held Draft (the `FEAT-0003` pattern)
captures the design without asserting an unproven causal claim. The experiment can
*kill* the lever — that is an acceptable outcome.

## 3. Risks & mitigations

- **R1 — `Global\Access_PCI` priority inversion (the serious one).** The tick does
  PawnIO `DeviceIoControl` under the system-wide `Global\Access_PCI` mutex — the same
  global lock named as the leading suspect for the 06-09 *whole-system* freeze under
  saturation (`cpu-0609-freeze-classification-2026-06-16.md` §1; the source rates this
  ~0.5 confidence, so it is the surviving hypothesis, not an established cause). A
  priority-15 thread contending that mutex on a saturated box is textbook inversion: a
  lower-priority cross-process *holder* (e.g. HWiNFO64) cannot get scheduled to release,
  and Windows' anti-starvation boost runs on a multi-second cadence. *What actually
  bounds the controller's exposure:* the elevated thread's wait on that mutex is itself
  bounded — `PciMutexLock` uses `WaitForSingleObject(handle, kPciMutexTimeoutMs ≈
  100 ms)` (`src/hardware/amd_reader.cpp`), so on a saturated box it parks ≤~100 ms, then
  the read degrades to a warning snapshot and fans hold last PWM (fail-safe). Note the
  no-busy-spin property is *necessary but not sufficient*: a clean kernel wait by our
  thread does not un-starve a foreign holder — it only keeps *our* exposure bounded.
  Whole-system protection is therefore the experiment (D-PRIO-4), which must watch
  system-wide harm, not just controller stall rate; if the inversion worsens
  whole-system behavior, the lever is rejected. *Mitigation summary:* bounded mutex wait
  + fail-safe degrade, blocking PawnIO/file I/O kept as kernel waits, and the
  no-busy-spin invariant on the elevated thread (REQ-PRIORITY-03/05).
- **R2 — recovery actors outranked.** Addressed by D-PRIO-3 — but only once the
  `--restart` killer process is elevated directly, not merely the watchdog task (raising
  the task `<Priority>` alone leaves the killer at base 8). Residual: the 06-09
  whole-system class freezes the killer too, where neither priority nor co-elevation
  helps — that class is outside this lever's and FEAT-0008's envelope (it is the Layer-1
  hardware-fallback domain).
- **Misaimed lever.** If the stall is file-lock-bound (the plan's view), priority
  yields no measurable benefit. *Mitigation:* D-PRIO-4 makes a null/negative result a
  first-class, acceptable outcome that keeps the lever held or rejects it.

## 4. Rollback

The mechanism is contained: the config key set to `inherit` (or `normal`) plus a
worker relaunch restores the inherited-priority behavior with no schema or config
migration. The recovery-actor elevation (supervisor + `--restart` killer) is in-code
and gated by the same build; any watchdog-task `<Priority>` change reverts by
re-running the install script. No actuation or runtime-home schema is affected.
