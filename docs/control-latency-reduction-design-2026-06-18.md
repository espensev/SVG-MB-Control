# Control latency reduction — design & directions (2026-06-18)

**Status:** Proposed (design capture; not implementation authorization)
**Owns directions for:** `docs/features/FEAT-0017-faster-fan-reaction-under-load.md`,
`docs/features/FEAT-0018-adaptive-cadence-enablement.md`,
`docs/features/FEAT-0019-sidecar-persist-off-hot-path.md`
**Companion to:** `docs/CONTROL_LOOP.md`, `docs/CONTROL_PIPELINE_MATH.md`,
`docs/response-evaluation-tuning-plan.md`, `docs/MEASUREMENT_GATE.md`,
`docs/RUNTIME_HOME.md`

> This is a dated design record (per `docs/features/README.md` §3 promotion gate 3
> pattern). It captures the latency audit and proposes three directions. It does
> **not** authorize code: each owning `FEAT-*` spec stays `Draft` until its
> direction is accepted and (where it applies) its measurement-gate or
> response-evaluation evidence exists. `must`/`should`/`is` are used per
> `CLAUDE.md`; current behavior is cited from source.

## 1. Why

A latency audit of the control path (2026-06-18) asked two questions: how is
"fan reaction under load" produced today, and what raises it. The reaction time
from a temperature change to a commanded duty change decomposes into three terms,
each governed by a different mechanism. The audit found the highest-leverage
mechanisms are either dormant or conservatively tuned, and that the term most
people would tune first is not the binding one.

## 2. The reaction budget (current shipped behavior)

End-to-end reaction = **detect** + **shape** + **write**. Walking a CPU load step
on channel `0` (`release/control.json`; curve maps `50 C -> 15.5%`,
`84 C -> 66%`, so raw demand steps `dDemand ~= 50%`):

| Term | Mechanism (source) | Shipped value | Effect on the step |
|---|---|---|---|
| **Detect** | in-process sample each tick; `poll_tick_ms` (`src/control/tick_runner.cpp` `RunControlTick`) | `250 ms` fixed | `<= 250 ms` (avg `~125 ms`) |
| **Shape — EMA** | `demand_smoothing_rise_alpha`, per-tick on the rise path, **not** time-normalized (`src/control/channel_evaluator.cpp` `ApplyDemandSmoothing`, rise branch) | `0.008`–`0.018` | sets the *target* the rate limiter chases; governs the approach **tail**, not the bulk of a step |
| **Shape — rate limit** | `rise_rate_pct_per_min` and `max_setpoint_step_pct` (`src/control/channel_evaluator.cpp` `RateLimitSetpoint`) | `75 %/min` and `0.6 %` | `1.25 %/s` — binds the **bulk** of the ramp (first `~32 s`, to `~55%`); the EMA tail governs the approach to `~66%` thereafter |
| **Shape — boost overlays** | four additive per-second integrators (`thermal_pressure` / `midband_pressure` / `gpu_airflow` / `cpu_low_soak`) plus `low_band`, summed into the target before the rate limiter (`src/control/boost_stage.cpp` `UpdateBoostStage`; `channel_evaluator.cpp` `ComputeFinalSetpoint`) | `0.12`–`0.25 %/s` own-rates | a second slow contribution to the target; hidden under the `1.25 %/s` main limiter today, but binds the boosted tail once the main ceiling is raised (FEAT-0017 §3) |
| **Gate** | `deadband_pct`, `write_cooldown_ms` | `0.25%`, `250 ms` | negligible / `=` tick |
| **Write** | atomic sidecar `Persist()` then `ApplyDuty` (`src/control/channel_write.cpp` `TryApplyChannelSetpoint`; `src/runtime/pending_writes.cpp` `Upsert`/`Persist`; `src/runtime/json_io.cpp` `WriteJsonFileAtomic`) | per **changed** write | one fsync'd file-replace on the critical path, ahead of the hardware write |

> The top-level `poll_ms = 500` in `release/control.json` drives the passive
> read / evidence-logging loop (`src/runtime/read_loop.cpp`,
> `src/runtime/evidence_log.cpp`), **not** the actuation tick. The control loop
> samples in-process every `poll_tick_ms` (`tick_runner.cpp` `RunControlTick`), so
> `poll_ms` is deliberately off the reaction path.

### 2.1 The rate limiter binds the bulk of a step; the EMA governs the tail

This is the load-bearing finding, and it splits the ramp into two segments. For a
load **step**, raising `demand_smoothing_rise_alpha` alone cannot speed the bulk of
the reaction:

- Rate-limit allowance per tick `= 75 %/min x 250 ms = 0.3125 %/tick = 1.25 %/s`.
  `max_setpoint_step_pct = 0.6` does not bind here (`0.6 > 0.3125`).
- `RateLimitSetpoint` chases the EMA-smoothed target, and the written duty
  (`last_issued_pct`) lags it. Simulating ch0 (`alpha = 0.012`, raw demand `66%`,
  start `15.5%`, 4 ticks/s): `last_issued_pct` is **rate-limiter-bound for the
  first `~32 s`**, climbing at `1.25 %/s` to `~55%`. Past `~32 s` the gap to the
  smoothed target is within the per-tick allowance, so the written duty equals the
  smoothed target each tick and the **EMA tail — not the rate limiter — governs**
  the approach from `~55%` toward `66%`. Because the EMA asymptotes from below, full
  `66%` is reached only asymptotically (`~59%` at `40 s`, `~63%` at `60 s`); the
  pure-rate-limit estimate `(66-15.5)/1.25 = 40 s` describes the rate-bound segment,
  not arrival at `66%`.
- So the two levers act on different segments. **Raising `alpha` cannot speed the
  rate-bound bulk** (the first `~55%`) — only raising `rise_rate_pct_per_min` +
  `max_setpoint_step_pct` does. Raising `alpha` speeds only the **tail**
  (`~55% -> 66%`): for `alpha = 0.012 -> 0.08` the EMA target moves through one
  time constant (`~63%` of the step) in `~3 s` and to within a few percent of the
  target by `~10 s`, while the rate-limited bulk is unchanged. So a bigger `alpha`
  shortens the settle, not the spike response.

A useful identity for tuning (steady writing at the cooldown cadence):

```
effective rise ceiling (%/s)
  = min( rise_rate_pct_per_min / 60 ,
         max_setpoint_step_pct x 1000 / write_cooldown_ms )
```

Shipped channel `0`: `min(75/60, 0.6 x 1000/250) = min(1.25, 2.4) = 1.25 %/s`.
Past `rise_rate_pct_per_min ~= 144` the `max_setpoint_step_pct` cap (`2.4 %/s`)
becomes binding, so **both** values must be raised together to go faster.

### 2.2 What is already implemented for latency-under-load

Worth keeping, and the reason the levers below are "enable/tune," not "build":

- **1 ms timer resolution** (`TimerResolutionScope`, `src/control/control_scheduler.cpp`)
  so the fixed period is not stretched by the `~15.6 ms` scheduler quantum.
- **Adaptive cadence engine** (`src/control/cadence_score.cpp` `ComputeCadence`):
  tightens the tick toward `poll_tick_floor_ms` as CPU/GPU slew rises, tighten
  instant, relax rate-limited. **Inert in the shipped config** — no
  `poll_tick_floor_ms`, so `F >= P` short-circuits it (FEAT-0018).
- **Hot-path I/O minimised**: status JSON every 10 ticks, `current_state.json`
  every `>= 1 s`, low-band evidence every `5 s`, sidecar *removals* batched once
  per tick — only *upserts* persist synchronously (FEAT-0019).
- **Write-path resilience** (FEAT-0010–0013): persist failure no longer vetoes the
  write; corrupt sidecar quarantined; source-aware dropout safe mode; half-open
  breaker probe. These bound *tail* latency under fault, not the nominal path.
- **PCI-mutex batching** (`src/hardware/amd_reader.cpp`): one `Global\\Access_PCI`
  acquire per tick for the whole Tctl+CCD read, but with a `100 ms` timeout — a
  competing tool holding that mutex blocks up to `100 ms`, and on timeout that
  tick's CPU read is **dropped** (`amd_reader.cpp:852-857` returns a warning
  snapshot with no fresh Tctl; fans hold last PWM), deferring fresh detection to the
  next tick that acquires the mutex — so the added detection latency can exceed one
  tick. Noted as an external-contention factor; not addressed here.

## 3. Directions

### D-REACT-1 — Raise the binding rise constraint, lane-targeted, asymmetric
Owning spec: **FEAT-0017** (`REQ-REACT-*`). Raise `rise_rate_pct_per_min` **and**
`max_setpoint_step_pct` jointly on the lanes where reaction matters (radiator lanes
`1`/`4`/`5` first), and raise `demand_smoothing_rise_alpha` for the approach tail.
Keep the falling direction at least as slow (fast rise, slow fall) to preserve
quiet spin-down. This is config-only: no curve breakpoints, channel set, cadence,
or control-identity change, so it does **not** move the `MEASUREMENT_GATE.md`
baseline; it is governed by `response-evaluation-tuning-plan.md` and validated
through its Pass 1–3 acceptance band. **Status: Proposed.** Held until a
Cinebench + max-CUDA pass measures before/after reaction and confirms the
acoustic/temperature envelope.

### D-CADENCE-1 — Enable the dormant adaptive-cadence floor
Owning spec: **FEAT-0018** (`REQ-CADENCE-*`). Set `poll_tick_floor_ms` below the
shipped `250 ms` and tune `cadence_slew_start_c_per_s` /
`cadence_slew_full_c_per_s` / `cadence_relax_per_s` so the loop halves detect
latency during a real thermal transient and idles at `250 ms` otherwise. The
engine already exists; this enables and characterizes it. **Crosses
`MEASUREMENT_GATE.md`** ("adaptive cadence floors below the shipped profile"), so
characterization evidence is required before implementation. **Status: Proposed
(held at the measurement gate).**

### D-WRITEHOT-1 — Gate the synchronous sidecar persist on baseline identity
Owning spec: **FEAT-0019** (`REQ-WRITEHOT-*`). `PendingWritesStore::Upsert`
persists on every changed write today; during a ramp the setpoint changes nearly
every tick, so an fsync'd atomic file replace sits on the critical path ahead of
`ApplyDuty` each tick — the documented Layer-0 stall surface under load. Crash
recovery (`src/runtime/write_orchestrator.cpp` `ReconcilePendingWrites`) restores
from `channel` + `baseline_duty_raw` + `baseline_mode_raw` **only**, and health
(`src/runtime/runtime_health.cpp`) reads only the sidecar's readability — neither
reads `target_pct`. So the **synchronous** `Persist()` can be gated on a change to
that recovery-relevant identity (first activation or baseline re-capture) and
same-baseline churn deferred to the existing once-per-tick end-of-tick `Flush()`, so
no fsync'd file-replace runs before `ApplyDuty` during a ramp — with zero change to
recovery behavior.

Cross-feature mechanism for the FEAT-0010 persist-failure counter (REQ-WRITEHOT-06),
decided at implementation: `Upsert` returns a `bool persisted`, and
`channel_write.cpp` clears `consecutive_sidecar_persist_failures` only when that is
true (fixes the *false clear*: a deferred same-baseline `Upsert` no longer resets the
counter while a failed activation record is still missing). Because `Persist()`
rewrites the **whole** sidecar, a successful end-of-tick `Flush()` means every
channel's record is on disk; so `Flush()` also returns a `bool`, and `tick_runner`
clears the persist-failure counter for all `context.channels` when it reports a
successful persist (fixes the *stuck-degraded* case: a failed activation that
self-heals through the batched deferred write). Both reset points are required: an
identity-change `Upsert` sets `dirty_ = false`, so a Flush-only reset would no-op and
never clear it. This is the build-ready direction: behavior-preserving, code-local,
not gate-crossing. **Status: Current (accepted 2026-06-18).**

### Not promoted here
- **Lower the fixed `poll_tick_ms`/`write_cooldown_ms`** — same gate as
  D-CADENCE-1, higher steady cost; the floor approach captures most of the benefit
  first. Deferred.
- **Worker priority elevation** — already `docs/features/FEAT-0009-controller-priority-elevation.md`
  (`REQ-PRIORITY-*`, Draft/held). Reduces tail latency under load; cross-ref only.

## 4. How any change is measured

The control-loop CSV already logs `last_raw_demand_pct`,
`last_smoothed_demand_pct`, the final setpoint, `channelN_feedforward_pct`/
`channelN_correction_pct`, plus `loop_slip_ms`, `loop_overrun`, and
`cadence_transient` (`docs/CONTROL_LOOP.md` Outputs). So EMA lag, the rate-limiter
ceiling, and loop slip under the faster floor are directly observable from a live
capture analyzed with `svg-mb-control analyze ingest` + `analyze report` — the
before/after instrument for every direction here.

## 5. Open decisions (carried into the owning specs)

| Decision | Direction | Needed before | Current lean |
|---|---|---|---|
| Which lanes and target ramp time for the retune | D-REACT-1 | implementation | radiator lanes `1`/`4`/`5`; a `~6 s` 40%-step ramp candidate, not chosen |
| The characterized floor value and slew thresholds | D-CADENCE-1 | implementation (after the gate pass) | `125 ms` floor candidate; thresholds from the characterization run |
| Whether to add a debug counter for skipped persists | D-WRITEHOT-1 | implementation | omit; the existing event/CSV telemetry is enough |
