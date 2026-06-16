# 2026-06-09 Control-Loop Freeze — Mechanism Classification — 2026-06-16

Status: **analysis record (neutral).** Classifies the 2026-06-09 whole-system
freeze against the documented record and decides whether the protocol §5 AVX-512
power-virus escalation tests the FEAT-0008 natural-hard-freeze premise. It draws no
control-path code or `control.json` change (`AGENTS.md` §Feature Intake Gate).
Method: an 8-agent adversarial workflow (4 evidence lenses → synthesis → 3 skeptics
including a steelman *for* running the virus) over the primary record, `src/`, and
the 687k-line `release/runtime/logs/svg_mb_control_events.jsonl`. Companion to
`docs/cpu-loop-stall-reproduction-protocol-2026-06-15.md` (the method + §5),
`docs/cpu-loop-stall-reproduction-findings-2026-06-16.md`,
`docs/cpu-loop-survival-live-sweep-findings-2026-06-16.md`,
`docs/cpu-loop-survival-layer0-plan-2026-06-16.md`,
`docs/cpu-peak-temp-excursion-2026-06-09.md`, and
`docs/features/FEAT-0008-watchdog-hung-worker-recovery.md`.

## 1. Classification — multi-factor whole-system co-freeze (confidence ~0.5)

The 06-09 event is a whole-system co-freeze ("control loop + system + HWiNFO/LHM all
stopped updating under full 32-thread saturation"; protocol §1) that does not
reproduce today at the shipped `BelowNormal` priority. Two single-factor hypotheses
are ruled out by primary evidence:

- **Thermal clock-collapse — ruled out as the freeze mechanism.** The worker ran at
  full cadence straight through the 04:42:42 ~107.1 °C Tctl peak (24
  `control_loop.write_applied` in that one second; no `worker_exited`, no restart, no
  staleness gap near the peak; the next `supervisor.start` was ~1h08m later, a
  routine recycle). No THERMTRIP; the system ran straight through. An SMU throttle
  cuts **cycles-per-slice** (clock/voltage), not **slices-per-second** (OS
  scheduling), so it cannot manufacture the worker-specific zero-scheduling gap a hard
  freeze requires. (`cpu-peak-temp-excursion-2026-06-09.md` §4;
  `events.jsonl` 2026-06-09 04:42 window.)
- **Pure worker-specific scheduling starvation — ruled out.** HWiNFO/LHM run at
  Normal priority while the worker is `BelowNormal`; all three co-freezing cannot be
  a worker-*only* scheduling miss. (brief §The stall; protocol §1.)

What remains is multi-factor: 32-thread saturation as the trigger plus a shared
low-level path (`Global\Access_PCI` mutex / `DeviceIoControl`, the common
hardware-access route for all three victims) co-freezing, and/or contention crossing
the staleness threshold. The specific `pending_writes.json` file-replace race did
**not** fire on 06-09: zero `control_loop.sidecar_upsert_failed` events on
2026-06-09 (of 73,521 dated events) vs 1,417 whole-log. So this is the
shared-driver/saturation reading of the I/O category, **not** the file-replace race.

Open: the freeze window left no captured in-band trace (a true total freeze halts the
delta-triggered logger too), so the co-freeze property rests on operator recollection
plus the absence-of-trace; the exact shared-path mechanism on 06-09 is not directly
captured.

## 2. AVX-512 power-virus is the wrong test for the FEAT-0008 premise

FEAT-0008's force-terminate fires **only** on `stop_result==2` — a 15 s graceful-stop
*timeout* (`control_supervisor.cpp:413-417`; `app_main.cpp:191-209`). Reaching it
requires the worker's control thread to fail a trivial atomic-bool-load +
`stop.request.json` `filesystem::exists()` check for 15 s
(`control_loop.cpp:174-175`; `control_scheduler.cpp:175-192`, 50 ms poll slice;
`runtime_lifecycle.cpp:25-29`). Four verified arguments — the asymmetry one decisive
— establish that a thermal virus cannot produce that:

1. **Asymmetry (decisive).** `stop_result==2` needs the worker frozen 15 s *while the
   watchdog stays healthy* to write `stop.request.json` and let its own 15 s
   `steady_clock` deadline fire. Uniform heat hits the `BelowNormal` watchdog equally
   and cannot selectively freeze one process. A 06-09-style whole-system freeze would
   stall the watchdog too → post-recovery restart, not a clean graceful-stop timeout.
   The thermal signature is nearly the **opposite** of the FEAT-0008 trigger.
2. **Timer-bound poll.** The inter-tick wait is `wake_cv.wait_until` on a
   `steady_clock` deadline under `timeBeginPeriod(1)`. SMU throttle slows instruction
   retirement, not `steady_clock`/QPC or the OS timer that wakes the thread, so a
   throttled worker stays runnable-and-scheduled and keeps polling.
3. **Magnitude.** SMU throttle is at most a ~2-3× clock reduction; a 15 s
   single-tick/poll miss is 2-3 orders of magnitude beyond any throttle effect.
4. **Empirical.** The live AVX2 sweep (`high+pin+oversubscribe×4`, peak 76.4 °C)
   produced 0 force-terminations; the worker honored every stop (`exit_code=0`).
   AVX-512 differs from AVX2 only in power/heat — the same throttle channel — so more
   heat adds no new path to a 15 s miss.

**Corollary (kill-shot): even a *successful* AVX-512 reproduction of the 06-09 class
would be the wrong freeze.** A whole-system freeze also freezes the watchdog, which
then cannot fire `--restart`/force-terminate; FEAT-0008 requires a *worker-specific*
freeze with a *healthy* watchdog — exactly what the `NtSuspendProcess` proxy
simulates. So the virus cannot validate FEAT-0008 whether or not it "works."

## 3. The natural-hard-freeze premise across all observed axes

No natural `stop_result==2` has ever occurred: the only one in the 687k-line log is
the artificial `NtSuspendProcess` suspend (exactly one `worker_force_terminated`,
`exit_code=1`; live-sweep Appendix B). Every natural stall produced graceful
recovery:

- **CPU starvation:** AVX2 `high+pin+os×4` → degrade + graceful recycle,
  `exit_code=0` (live-sweep §3.2). Windows balance-set-manager boost caps a
  runnable `BelowNormal` thread's starvation at a few seconds, well inside 15 s.
- **I/O / driver:** the 2026-06-15 NDIS hang *advanced* `tick_count`
  (22452→22455 in ~25 s) and recovered gracefully (`shutdown_restore_applied` on all
  6 channels) — slow-but-alive, not a `stop_result==2`.

So both observed natural axes yield graceful recovery; the only producers of a 15 s
stop-miss are a deterministic deschedule (suspend) or a hard kernel-wait block —
neither thermal.

**Direct I/O write-path probe (confirmatory, 2026-06-16).** To close the I/O axis
directly rather than only by mechanism + the NDIS precedent, an external read-share
handle (`FileShare.Read`, blocking the atomic rename-replace, allowing the
controller's reads) was held on `release/runtime/pending_writes.json` on the live
loop under a brief 6-thread nudge (`Probe-SidecarLock.ps1`;
`sidecar-lock-probe-20260616.json`). Result: **23 `control_loop.sidecar_upsert_failed`
events (fan-write skip), `tick_count` advanced throughout (~0.83/s vs 4/s nominal —
degraded but responsive), 0 force-terminations, 0 restarts, worker pid unchanged**;
on release, `write_applied` resumed immediately with 0 residual sidecar errors. This
**confirms** (does not discover — the mechanism is what proves it) that a write-path
I/O stall is the documented *safe-degraded* state (hold-last-PWM) with the worker
*responsive*, not frozen. The probe was deliberately bounded by an active
tick/status-freshness release (fired at status-age 6.3 s, well under the 10 s floor):
status-age reaching 6.3 s in 7.6 s of lock empirically shows a *sustained* lock would
trip the 10 s staleness → a **graceful** watchdog recycle (the worker honors the stop
because it is responsive), **not** a `stop_result==2` — and the release intentionally
avoids exercising the un-characterized restart-reconcile-during-lock path. A
zero-live-risk assertion of the same skip-not-block invariant is available as the
`RecordingFanWriter` unit test specced in the Layer-0 plan §6 (not added here).

## 4. Recommended next step — close the scheduling axis per protocol §5 (not the virus, not yet a reframe)

> **Update 2026-06-16 (post-run): the recommended step ran; the reframe is now
> earned.** The aggressive cell (`--oversubscribe 4 --priority high --pin --seconds
> 45`) was repeated **n=5** against the live `BelowNormal` FEAT-0008 controller:
> **5/5 SURVIVE, 0 force-terminations, 0 graceful restarts, 0 `sidecar_upsert_failed`,
> peak 76.1 °C** (worker pid 44676 unchanged; independently cross-checked against
> `events.jsonl` — only 1,940 `control_loop.write_applied` in the window). With the
> prior n=1 (cell C2) this is **n=6 total** at the most aggressive cell and **zero**
> natural `stop_result==2`. Protocol §5's "across repeats" precondition is met. The
> close rests on the §2 mechanism (timer-bound stop poll + balance-set boost +
> watchdog asymmetry); the n=6 **corroborates** it — 0/6 is consistent with, not a
> tight rate bound on, the per-cell hard-freeze rate (rule of three, ~50 % upper
> 95 % CI). So the §1 reframe is mechanism-grounded and load-corroborated, not
> "proven impossible by six runs." Record:
> `docs/cpu-loop-survival-live-sweep-findings-2026-06-16.md` Appendix C;
> `release/runtime/experiments/loop-stall/aggressive-repeat-results-20260616.json`.


- **Do not run the AVX-512 virus** (§2): wrong instrument, and a successful
  06-09-class reproduction is outside FEAT-0008's recovery envelope anyway.
- **The protocol §5 precondition is not yet met.** §5 gates escalation on the
  aggressive AVX2 cell failing *across repeats*; the live sweep ran it at **n=1**
  ("cannot estimate the stall rate", live-sweep §4 item 3 / §5). The cheap,
  low-risk, already-built step is to **re-run the aggressive cell
  (`high+pin+oversubscribe×4`) at n>1** (default-OFF `cpu-synth-load`, bounded 45 s
  cells, operator-present, 76.4 °C envelope) to convert n=1 into a probability
  estimate. This was the point the workflow's own action-skeptic raised against a
  premature reframe.
- **Then ground the conclusion in that result.** The mechanism analysis predicts
  graceful recovery on every repeat (balance-set boost rescues the runnable worker);
  if so, the scheduling axis is legitimately closed and the natural-hard-freeze
  premise is reframed on evidence: not reproducible by load on this system; FEAT-0008
  is validated defense-in-depth for the worker-specific deschedule / kernel-block
  class, with the system-wide class out of its recovery envelope. The I/O/write-path
  axis is the next-most-plausible natural producer but has itself only ever recovered
  gracefully.

## 5. Method / caveats

- 8-agent adversarial workflow. The synthesis's first-pass "reframe now"
  recommendation was itself **refuted** by the action-skeptic — for skipping the §5
  n>1 precondition and for a misread (the NDIS hang was *graceful*, not a hard
  freeze, so it does not "partially evidence" the premise). §4 reflects the corrected
  position: virus rejection stands; the immediate step is the n>1 AVX2 repeat, not a
  reframe.
- 06-09 has not rotated out (73,521 dated events present) but the freeze window
  itself is a logging gap.
- The thermal contribution is made **implausible** by the timer/asymmetry argument,
  not **falsified** by a contemporaneous clock measurement (APERF/MPERF +
  package-power logging postdate 06-09; clock-loss magnitude unquantified).

## 6. References

- Method + §5 escalation: `docs/cpu-loop-stall-reproduction-protocol-2026-06-15.md`.
- First repro pass: `docs/cpu-loop-stall-reproduction-findings-2026-06-16.md` (§2.1
  code-confirmed mechanism; §3 A1 graceful timeline).
- Live FEAT-0008 sweep: `docs/cpu-loop-survival-live-sweep-findings-2026-06-16.md`
  (§3-§5, Appendix B attribution).
- Layer-0 levers: `docs/cpu-loop-survival-layer0-plan-2026-06-16.md` (§3.2 L0-A4
  response algorithm; §3.3 L0-B priority lane deferral).
- Thermal event: `docs/cpu-peak-temp-excursion-2026-06-09.md`.
- Feature: `docs/features/FEAT-0008-watchdog-hung-worker-recovery.md`.
- Key source: `src/control/control_supervisor.cpp:399-423`,
  `src/app/app_main.cpp:191-209`, `src/control/control_loop.cpp:174-175`,
  `src/control/control_scheduler.cpp:158-193`, `src/runtime/runtime_lifecycle.cpp:25-29`.
