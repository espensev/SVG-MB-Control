# CPU Control-Loop Stall — Reproduction Findings — 2026-06-16

Status: **measurement record (neutral).** Records the inputs, method, and numeric
results of the first stall-reproduction pass run under
`docs/cpu-loop-stall-reproduction-protocol-2026-06-15.md`. It draws no design
conclusion; Layer-0 sizing is owned by Phase 2 and the maintainer. `AGENTS.md`
§Feature Intake Gate held throughout: the controller was the shipped build at its
shipped `BelowNormal` priority (no fix applied), and the only added component is
the default-OFF, read-only synthetic load generator. FEAT-0001 / FEAT-0004
unaffected.

Scope:

- In scope: the instrument and detector parameters, the per-cell numeric results,
  and the reconstructed stall timeline with its worst-case bounds.
- Out of scope: any Layer-0 threshold/priority decision; the affinity-isolation and
  oversubscription cells (not yet run); the thermal excursion (hardware-owned).

## 1. Setup

- **Machine:** 32 logical CPUs. **Controller:** shipped build, `BelowNormal`
  priority, foreground worker under `--run-supervisor`; `poll_tick_ms = 250`
  (nominal cadence 4 ticks/s).
- **Instrument:** `cpu-synth-load.exe` (tooling commit `161f040`), all-core AVX2
  FMA saturator with the scheduling knobs from the protocol. Default-OFF build;
  read-only (no MSR/PCI/fan writes).
- **Detector:** `release/runtime/experiments/loop-stall/Measure-StallCell.ps1` reads
  `release/runtime/logs/svg_mb_control_events.jsonl` over the load window and
  reports tick cadence (`Δtick_count / Δt`), `control_loop.write_applied` count,
  max write gap, error/warn count, and a controller-PID-change (watchdog restart)
  flag. Raw evidence slice: `…/loop-stall/a1-stall-evidence-20260616.jsonl`
  (1378 events, 00:05:00–00:10:00).

## 2. Results

| Cell | Load | Cadence (ticks/s) | Writes | Max write gap | Controller | Verdict |
|---|---|---|---|---|---|---|
| Baseline | 32 threads, **normal**, no pin, 60 s | **1.12** (of 4) | 314 | 6 s | alive (pid stable) | **survives (degraded ~3.6×)** |
| Affinity | 32 threads, **normal**, **+pin**, 60 s | **0.85** (of 4) | 252 | 3 s | alive (pid stable) | **survives (degraded)** |
| A1 | 32 threads, **above**, no pin, 60 s | collapse → **stall** | — | **~25 s** | **watchdog restart** (pid 40956 → 53332) | **STALL** |

Idle/ambient cadence before each cell measured at ~4.0–4.1 ticks/s; the controller
recovered to ~4 ticks/s and ~44 °C within ~60 s after each cell.

**Discriminator: the load's priority class relative to the `BelowNormal`
controller — not core occupancy.** A `normal` load (one class above) survives at
~1 tick/s whether unpinned (1.12/s) or pinned one-per-core (0.85/s): pinning adds
only marginal degradation and **no** stall (no restart, 0 errors, ≤3 s write gap).
Elevating the same load one further class to `above` stalls it (watchdog restart,
~25 s dead-time). So affinity pinning alone is **not** sufficient — the stall is
driven by the load out-prioritising the controller. This isolates the Layer-0
lever to **the controller's own priority** (raise it from `BelowNormal` toward
Normal/High), with affinity / CPU-Sets as a secondary "harder-guarantee" measure,
consistent with the brief's 06-09 "operator-induced starvation" reading.

## 3. A1 stall timeline (reconstructed from `events.jsonl`)

Load started 00:08:45 (`above`, 32 threads).

| Time | Observation |
|---|---|
| 00:08:56–00:09:05 | Loop alive but cadence ~1 tick/s (9658→9666); temp ~83.6 °C |
| 00:09:06–00:09:09 | `CONTROL_LOOP_SIDECAR_UPSERT_FAILED` (×6) — write failures under starvation; tick stuck at 9667 |
| 00:09:09–00:09:20 | Intermittent recovery burst (~3 ticks/s) |
| 00:09:20 | `control_loop.shutdown_requested` + `shutdown_restore_applied` (all 6 channels) — **graceful** stop |
| 00:09:21 | `supervisor.shutdown`, `supervisor.worker_exited` |
| 00:09:21 → 00:09:29 | **8 s gap** — supervisor itself starved before relaunch (`supervisor.start` 00:09:29) |
| 00:09:32 | `supervisor.worker_started` |
| 00:09:43–00:09:45 | `control_loop.start`, `authority_reasserted`, `baseline_captured` (all channels) — control resumes at tick 1 |

**Worst-case bounds (this cell):**

- **Fan-actuation dead-time ≈ 25 s** (last setpoint ~00:09:20 → first new setpoint
  ~00:09:45). During it the SuperIO held the last PWM (~50 % duty); temperature
  held ≤ ~84 °C and did **not** run away; the SMU 95 °C throttle was not reached.
- Cadence path: 4/s (ambient) → ~1/s (under load) → stall.
- Failure signature under CPU starvation is the same `pending_writes.json`
  upsert-failure class seen in the 06-15 NDIS-hang incident (there caused by I/O
  stall, here by scheduler starvation).
- The recovery was the designed path: graceful channel restore → supervisor
  relaunch → authority reassert. The watchdog/supervisor recycle worked; the cost
  is the ~25 s dead-time, which is the quantity Layer-0 must reduce.

## 4. Not yet run / caveats

- **Oversubscription:** `--oversubscribe 2|4` — not run; would test stall
  *severity* (dead-time, repeated restarts), not the threshold, which is already
  isolated to priority.
- **Reproducibility:** A1 is n=1 (mechanism is fully logged, not a fluke, but a
  confirm-repeat would harden the "deterministic at `above`" claim).
- **Detector threshold:** cadence < 1 tick/s alone is *degraded*, not a stall — a
  `BelowNormal` loop sharing cores with a `normal` load runs at ~1/s yet keeps
  actuating. The reliable stall signals are a watchdog restart, writes stopping,
  or a long (≥ ~10 s) write gap; the harness verdict was corrected to require one.
- AVX2 reproduced a *scheduling* stall; it does not reach the 06-09 ~107 °C
  thermal regime. The protocol's §5 AVX-512 escalation was **not** triggered (a
  repro was obtained), and is reserved should a thermal-coupled question arise.

## 5. References

- Method: `docs/cpu-loop-stall-reproduction-protocol-2026-06-15.md`.
- Brief: `Suggestions/claude-brief-loop-survival-layer0-2026-06-15.md` (Phase-1
  reproduction gap; `control_thread_priority.h` as the Layer-0 priority starting
  point).
- Detector calibration: memory `cpu-controller-restart-ndis-hang-2026-06-15`.
- Evidence: `release/runtime/experiments/loop-stall/a1-stall-evidence-20260616.jsonl`.
