# Decision: a pending-write sidecar persistence fault must not veto fan actuation

**Project:** svg-mb-control
**Status:** Current (Accepted 2026-06-17)
**Owning feature:** `docs/features/FEAT-0010-write-actuation-sidecar-fault.md`
(`REQ-WRITESAFE-*`)
**Companion to:** `AGENTS.md`, `docs/WRITE_ORCHESTRATION.md`,
`docs/CONTROL_LOOP.md`, `docs/RUNTIME_HOME.md`, `docs/MEASUREMENT_GATE.md`

## Context

On the control hot path, `TryApplyChannelSetpoint` persists the pending-write
sidecar entry before it commands the fan:

```
src/control/channel_write.cpp:319-335
    try { pending_store.Upsert(entry); }
    catch (const std::exception& e) { AppendControlLoopEvent(... "control_loop.sidecar_upsert_failed" ...); return; }
src/control/channel_write.cpp:337-338
    const FanWriteResult write_result = fan_writer.ApplyDuty(channel.config.channel, setpoint);
```

`PendingWritesStore::Upsert` persists synchronously through the throwing
`WriteJsonFileAtomic` (`src/runtime/pending_writes.cpp:121-141`,
`src/runtime/json_io.cpp:187-255`). When the atomic replace cannot complete —
`ReplaceFileWithTemp` retries six times on `ERROR_SHARING_VIOLATION` /
`ERROR_ACCESS_DENIED` / `ERROR_LOCK_VIOLATION`, then throws
(`src/runtime/json_io.cpp:33-58`) — the `catch` returns before `ApplyDuty`. The
fan write is skipped.

This was finding H1 of the 2026-06-17 team review and is **runtime-reproduced**
(isolated, simulated hardware; evidence
`review/repro/h1_repro_evidence.json`, harness
`review/repro/h1_sidecar_veto_repro.py`). With `pending_writes.json` held open
share-read-only on the same rising-temperature ramp:

| Signal | CONTROL (no lock) | LOCKED |
|---|---|---|
| `control_loop.write_applied` events | 30 | 10 (frozen at lock) |
| `control_loop.sidecar_upsert_failed` events | 0 | 14 |
| Computed channel setpoint | 100% @ 99 C | 92% @ 89 C (still climbing) |
| Applied duty | tracked to 100% | frozen at 58.75% (pre-lock value) |
| `circuit_breaker_open` / `consecutive_write_failures` | false / 0 | false / 0 |
| Loop ticks during the lock window | — | 10 advanced |

Three consequences, all observed or established by code:

1. **The fan write is vetoed.** Duty froze at the pre-lock value (58.75%) while
   the curve demanded 92%+ under rising temperature — stuck low.
2. **The sensor-safe command is vetoed too.** The `safety_override` breaker
   bypass at `src/control/channel_write.cpp:307` is upstream of the Upsert gate
   at line 320, and the `catch` has no `safety_override` exemption, so a
   persistent sidecar fault also suppresses the 100% safe-mode write (code path;
   the simulator cannot express sensor loss cleanly, so this sub-claim is
   established by reading, not reproduced).
3. **No backstop.** The skip does not increment `consecutive_write_failures`
   (`src/control/channel_write.cpp:337-344` is never reached), so the breaker
   never escalates; and `control_runtime.json` is written by the **non-throwing**
   `TryWriteJsonFileAtomic` (`src/runtime/runtime_status.cpp:190`), so it stays
   fresh and the staleness watchdog (`src/runtime/runtime_health.cpp`) never
   recycles. The loop advanced 10 ticks while locked.

## Why the current order exists

The persist-before-act ordering is deliberate: it lets a crash mid-write leave a
recovery record from which the next worker start restores the captured baseline
(`src/runtime/pending_writes.cpp:136-139`; `docs/WRITE_ORCHESTRATION.md` Runtime
Flow step 6). The decision must preserve that crash-recovery guarantee for the
common path while removing the sidecar's veto over actuation.

## Options considered

- **A — best-effort persist, actuate anyway.** Keep `Upsert`→`ApplyDuty` on the
  happy path. On a persist failure, still call `ApplyDuty`, and surface an
  observable backstop. `safety_override` always actuates.
- **B — reorder: `ApplyDuty` before `Upsert`.** Simpler, but never persists
  intent before acting, weakening first-write crash recovery for *every* write
  and contradicting `docs/WRITE_ORCHESTRATION.md` step 6.
- **C — `safety_override` bypass only.** Smallest change, but leaves the proven
  normal-demand stuck-low case (consequence 1) unfixed; it does not close H1.

## Decision

**Adopt Option A.** Reasons:

- The durability concern is the captured **baseline**
  (`baseline_duty_raw` / `baseline_mode_raw`), which is stable across ticks. The
  reconcile/restore paths replay the baseline, not the per-tick `target_pct`
  (`src/runtime/write_orchestrator.cpp:341-355`,
  `src/hardware/sio_fan_writer.cpp` restore). A stale-but-present sidecar entry
  therefore restores correctly. Actuate-anyway loses only the latest
  `target_pct`, not recovery safety.
- `PendingWritesStore::Upsert` updates the in-memory `entries_` vector before
  `Persist()` throws (`src/runtime/pending_writes.cpp:125-140`), so the next
  successful tick re-persists the file once the lock clears — the store
  self-heals.
- Option A is strictly better than B: it keeps persist-before-act on the happy
  path and only diverges on the fault path.

**Backstop on actuate-after-persist-failure:** degrade the runtime **health**
state and keep the `control_loop.sidecar_upsert_failed` event, adding a
per-channel counter so the condition is observable and reviewable. Do **not**
escalate the write-failure circuit breaker (the actuation succeeded) and do
**not**, by this condition alone, drive a watchdog recycle (a recycle cannot
clear a persistent external file lock).

**Accepted residual:** if the *first* write to a channel has a failing persist
and the worker then crashes before any entry exists, the next worker has no
baseline record for that channel. This is narrow (atomic temp+rename means an
ordinary crash leaves a complete prior file; the trigger needs a sustained lock
plus a crash in the same window), the in-window commanded duty is a cooling
command (fail-safe-leaning), and the next worker re-establishes control. It is
accepted rather than mitigated in v1.

## Scope and gate

- **Scope: H1 only.** The neighboring write-path findings — the open breaker
  blocking rising-demand writes (`src/control/channel_write.cpp:300-309`) and
  restore/reconcile bypassing the blocked-channel guard
  (`third_party/SVG-MB-SIO/src/svg_mb_sio.cpp` restore) — are cross-referenced
  but **not** absorbed here; they are separate later rounds.
- **Measurement gate:** not crossed. The change affects only the failure path.
  The computed duty, cadence, channel set, and mixed-input strategy are
  unchanged, so there is no `docs/MEASUREMENT_GATE.md` baseline movement and no
  `docs/CONTROL_PIPELINE_MATH.md` identity change. Any new health/status field
  is additive to `docs/RUNTIME_HOME.md`.
- **Distinct from FEAT-0005:** FEAT-0005 (parked) detects a write the driver
  accepts but that does not actuate the fan, and its Phase 1 is read-only
  (`REQ-ACTCONFIRM-03` forbids changing write behavior). This decision is a
  behavior-changing fix to a write the controller *skips*, so it does not belong
  in FEAT-0005 and is not its gated Phase-2 escalation.
