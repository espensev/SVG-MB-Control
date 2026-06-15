# Control-Loop Survival (Layer 0) — Plan Scaffold — 2026-06-16

Status: **proposal / planning scaffold only.** Not authorized work (`AGENTS.md`
§Feature Intake Gate): no code, config, or behavior change is permitted from this
document. Each lever below is described up to, not through, a future
implementation-authorized FEAT. This is Phase 2 of the Layer-0 brief
(`Suggestions/claude-brief-loop-survival-layer0-2026-06-15.md`), sized by the
Phase-1 reproduction
(`docs/cpu-loop-stall-reproduction-findings-2026-06-16.md`). The line/function
citations below come from a code-grounding pass; the one load-bearing new claim
(§3.2, watchdog cannot recover a hung worker) was **spot-verified this session** (no
`TerminateProcess`/force-kill anywhere in `src/`; `app_main.cpp:190-197` returns
before relaunch when the stop times out).

Scope:

- In scope: the refined root-cause statement, the ranked Layer-0 levers with the
  invariants each must preserve, the deferred/rejected levers with reasons, and the
  validation method.
- Out of scope: writing any of it (gated); Layer-2 tuning; the thermal excursion
  (hardware-owned: SMU 95 °C throttle + SuperIO/EC fan fallback).

## 1. What Phase 1 + code grounding established (the root cause is not what the brief assumed)

The Layer-0 brief anticipated **thread priority** as the primary lever. Phase 1 and
the code grounding revise that:

- The control-loop stall is **probabilistic**, not a deterministic priority cliff:
  `above`-priority load stalled 1 of 3 runs; `normal` / `normal`+pin and the other
  `above` runs only degraded (~1 tick/s) and kept actuating.
- **A single transient error 5 cannot cause it.** Both `control_runtime.json` and
  `pending_writes.json` are written through `WriteJsonFileAtomic` →
  `ReplaceFileWithTemp` (`src/runtime/json_io.cpp:33`), which already retries
  `ERROR_ACCESS_DENIED`(5)/`SHARING`/`LOCK` 6× with exponential backoff (~630 ms).
  The original error-5 cause (readers opening without `FILE_SHARE_DELETE`, blocking
  the rename) was already fixed (`json_io.cpp:116-122`). Residual error 5 comes from
  *external* exclusive holders (AV / Windows Search indexer).
- The stall is the coincidence of two effects under sustained contention: (a) the
  sidecar `Upsert` throws error 5 *after* the 630 ms retry and **gates the fan
  write** — `TryApplyChannelSetpoint` returns before `fan_writer.ApplyDuty`
  (`src/control/channel_write.cpp:319-335`) — which is the **safe** state
  (fan holds last PWM); and (b) sustained starvation/blocking keeps
  `control_runtime.json` un-refreshed past the **10 s** staleness threshold, so the
  watchdog recycles the controller. Status is written every ~2.5 s
  (`kStatusUpdateIntervalTicks=10` × 250 ms, `src/control/tick_runner.cpp:396`), a
  ~4× margin to the 10 s threshold, so staleness also needs *sustained* failure.
- The watchdog is a **1-minute** scheduled task
  (`Install-SVG-MB-ControlWatchdogScheduledTask.ps1`), so the observed ~25 s
  dead-time is dominated by the 0–60 s poll phase, **not** the 10 s verdict.
- **Contract label correction:** the write-before-actuate crash-recovery rule lives
  in `docs/WRITE_ORCHESTRATION.md` + `write_orchestrator.cpp` + `pending_writes.cpp`
  + `app_main.cpp`, **not** FEAT-0001 (which is hot-swap write policy). Earlier notes
  that cited "FEAT-0001 write-once" mean this contract.

## 2. The layered model (from the brief, unchanged)

- **Layer 0 — loop survival** (the software work; §3). Keep the loop scheduled and
  its telemetry fresh under load, and recover it when it is not. Lives largely
  outside `control.json` (process/scheduling/watchdog surface).
- **Layer 1 — emergency response = hardware fallback** (§4), not a software clamp.
- **Layer 2 — tuning** (CO / Curve Shaper / PBO / EDC): operator-owned, out of scope.

## 3. Layer 0 — ranked levers

Ranked by the four-lens review (crash-recovery correctness, fail-safe,
proportionality, smallest-diff). Each lever names the invariants it must preserve.

### 3.1 L0-A1 — scoped sidecar-upsert retry (primary write-path fix)

Add a **small** bounded retry (1–2 extra attempts) at the sidecar `Upsert` call site
(`channel_write.cpp:319-335`) so a brief residual lock that clears within a tick no
longer skips the fan write. Drives the transient-error-5 stall contribution toward 0
while keeping `Upsert`-before-`ApplyDuty` ordering and the return-before-`ApplyDuty`
final-failure path.

Guards (non-negotiable, from the review):

- **Scope to the call site, not the shared primitive.** Do *not* widen
  `ReplaceFileWithTemp`'s retry count — it is shared by every status/snapshot/CSV/
  supervisor writer and would slow all of them; the single transient error 5 is
  already absorbed there.
- **Per-tick total sidecar-I/O cap.** `Persist()` is synchronous on the 250 ms
  control thread; the added backoff stacks on the existing ~630 ms. Cap *cumulative*
  per-tick sidecar I/O well under the `restore_timeout_ms` and the staleness margin,
  so the retry cannot itself provoke `kTimedOut`/abort or the 10 s recycle.
- **Marginal-benefit caveat.** A skipped write is already the safe state (hold-last
  PWM + 95 °C backstop), so A1's value is only reducing the probability that a
  *cluster* of skips coincides with the staleness trip — not a correctness fix.

### 3.2 L0-A4 — watchdog recovery of a genuinely hung worker (the real gap)

**Verified this session.** On `kStale` the watchdog runs `--restart` →
`RequestStopAndWait` (15 s; `control_supervisor.cpp:399-423`). A worker frozen hard
enough not to honor the stop sentinel within 15 s returns non-zero, and
`app_main.cpp:190-197` then **does not relaunch** (`if (stop_result != 0) return …;`
before `start_requested = true`) — and there is **no `TerminateProcess`/force-kill**
anywhere in `src/`. So a genuinely
hung worker (the 06-09 "everything stopped updating" case) is *detected but not
recovered*. Phase 1's A1 reproduced only the *recoverable* case (the worker honored
the graceful stop). 

Lever: after `RequestStopAndWait` times out, escalate to a force-terminate
(`TerminateProcess`) of the worker (and, if needed, the supervisor) before relaunch,
so a true freeze is recovered. This is the highest-value Layer-0 item the original
lever set missed (the non-recovery is confirmed; the live hung-worker path under load
is what to exercise during implementation).

### 3.3 Deferred / contingent

- **L0-C — staleness / dead-time sizing.** Affects recovery *latency*, not stall
  rate. The dead-time is dominated by the 1-minute watchdog poll (already at the
  `IntervalMinutes` floor of 1), which staleness sizing cannot shrink. Tightening the
  10 s threshold raises false-restart risk and **interacts adversarially with A1**
  (A1 lengthens worst-case tick latency). If taken, keep
  `staleness_threshold_ms ≥ measured single-tick worst case` (tick + ~630 ms +
  A1 budget); use only to *bound*, never minimize. Size jointly with A1.
- **L0-B — controller priority elevation.** Misaimed at this root cause:
  `THREAD_PRIORITY_TIME_CRITICAL` improves CPU scheduling, but the residual error 5
  is a **file-lock** wait, which priority does not shorten — and elevating the
  control thread can *delay* a CPU-bound external lock holder from releasing the
  handle. It is also priority-15 (system-wide starvation reach) and net-new code
  (`control_thread_priority.h` does not exist), disproportionate for a rare
  operator-induced event. Revisit only with measured contention evidence on a
  divergent-load session, as defense-in-depth *after* A1.
- **L0-A3 — harden the status-freshness writer.** Targets a **non-cause**: the
  status writer already has the 6-retry + a ~4× cadence margin and its return value
  is intentionally ignored; the stall is the sidecar gating the fan write, not the
  status write failing. Any A3 must keep the atomic temp+`MoveFileExW` protocol
  (an in-place write would turn a restartable `kStale`/exit 2 into a permanently
  stuck `kFailed`/exit 3) and must keep freshness coupled to real tick completion
  (forging freshness would blind the watchdog). Defer.

### 3.4 Rejected

- **L0-A2 — decouple actuation from the sidecar gate.** **Reject (all four lenses).**
  Best-effort-sidecar-then-actuate reintroduces the exact unsafe state the gate
  forbids: `{durable hardware override + no durable baseline record}`. A crash after
  `ApplyDuty` with no sidecar entry leaves the SIO chip holding the override with the
  baseline lost and no `ReconcilePendingWrites` recovery path. The safety-command
  bypass does not rescue it. Never ship.

## 4. Layer 1 — hardware fallback (per brief)

Verify/configure the SuperIO host-write/fan watchdog and SmartFan auto curve so fans
reach a safe state on host silence, with the SMU die-temp throttle as primary. Open
input: the NCT6701D register map (a host-silence fan-revert/timer register; per-fan
manual-vs-auto mode bits; any critical-temp/full-speed register). Hardware
configuration + verification, not a software stage.

## 5. Layer 2 — out of scope

CO / Curve Shaper / PBO / EDC are the operator's levers for transient temperatures;
referenced, not built here.

## 6. Validation

- **A1 unit test (no fault-injection seam exists yet):** hold an exclusive handle on
  `RUNTIME_HOME/pending_writes.json` *without* `FILE_SHARE_DELETE` to drive the
  genuine error-5 path, then with the `RecordingFanWriter` harness
  (`tests/cpp/channel_write_tests.cpp`) assert: (a) the sidecar entry is durable
  before any `ApplyDuty`; (b) under a lock that clears within budget, `ApplyDuty` is
  invoked exactly once (stall contribution → 0); (c) under a lock that persists past
  budget, `ApplyDuty` is not called and the tick still completes within
  `stale_after_ms` (dead-time bounded). Add a latency assertion that worst-case
  Upsert backoff + tick work stays under the C-sized staleness threshold (proves A1
  and C do not interact into a self-inflicted restart).
- **A4:** reproduce a true freeze (e.g. a held lock / deliberate block longer than
  15 s) and show the force-terminate escalation recovers the worker.
- **System:** re-run the reproduction protocol
  (`docs/cpu-loop-stall-reproduction-protocol-2026-06-15.md`) repeatedly; because the
  stall is probabilistic, success = stall rate → ~0 across many `above` runs (and/or
  dead-time bounded), not a single pass.

## 7. Invariants any Layer-0 change must preserve

1. **Write-before-actuate:** the sidecar entry is durable before `ApplyDuty`
   (`channel_write.cpp:319-335`, `write_orchestrator.cpp` RunWriteOnce). Forbids
   `{durable hardware override + no durable baseline}`.
2. **Atomic, never-in-place writes:** temp + `FlushFileBuffers` +
   `MoveFileExW(REPLACE|WRITE_THROUGH)` for both `control_runtime.json` and
   `pending_writes.json`. Keeps a transient failure on the restartable `kStale`/exit-2
   path, not the stuck `kFailed`/exit-3 path.
3. **Freshness coupled to real tick completion** — never forge
   `loop_last_evaluation`.
4. **Priority:** only-raise-never-lower; **no `REALTIME_PRIORITY_CLASS`**.
5. **Fail-safe floor:** fans hold last PWM on stall; SuperIO/EC + SMU 95 °C backstop
   unaffected by any software lever.

## 8. References

- Phase-1 findings: `docs/cpu-loop-stall-reproduction-findings-2026-06-16.md`;
  method: `docs/cpu-loop-stall-reproduction-protocol-2026-06-15.md`.
- Brief: `Suggestions/claude-brief-loop-survival-layer0-2026-06-15.md`.
- Crash-recovery contract: `docs/WRITE_ORCHESTRATION.md`,
  `src/runtime/write_orchestrator.cpp`, `src/runtime/pending_writes.cpp`,
  `src/app/app_main.cpp`.
- Key sites: `src/control/channel_write.cpp:259-345` (gate),
  `src/runtime/json_io.cpp:33-52` (shared retry),
  `src/runtime/runtime_health.cpp:144-244` (staleness/exit codes),
  `src/platform/task_runner.cpp:195-204` (watchdog),
  `src/control/control_supervisor.cpp:399-423,539-669` (stop-wait + restart loop).
- Incident corroborating the shared mechanism:
  `docs/cpu-peak-temp-excursion-2026-06-09.md` and memory
  `cpu-controller-restart-ndis-hang-2026-06-15`.
