# CPU Control-Loop Stall — Reproduction Protocol — 2026-06-15

Status: **method/protocol record (neutral).** This document defines how to
reproduce, and how to detect, the 2026-06-09 control-loop starvation stall so that
Layer-0 thresholds are sized from evidence rather than guessed. It draws no design
conclusion. `AGENTS.md` §Feature Intake Gate applies: this is characterization
tooling and method only — **no control-path code, no `control.json` behavior
change, no new schema.** The instrument it uses (`tools/cpu_synth_load.cpp`) is the
default-OFF, read-only synthetic load generator; the live controller is held at its
**shipped `BelowNormal` priority** for every cell (the Layer-0 fix is *not*
pre-applied — this measures the baseline the fix must beat). It serves Phase 1 of
the Layer-0 brief (`Suggestions/claude-brief-loop-survival-layer0-2026-06-15.md`)
and the starvation finding in `docs/cpu-thermal-emergency-clamp-plan-2026-06-15.md`.
FEAT-0001 (write policy) and FEAT-0004 (hardware/sensor-health fail-safe) are
unaffected.

Scope:

- In scope: the question, an objective stall detector with its metric and
  provisional thresholds, the load instrument and its knobs, a one-variable-at-a-
  time sweep, and a falsifiable escalation path.
- Out of scope: any Layer-0 threshold/priority *decision* (Phase 2), any
  `control.json` change, any control-path code, and the thermal excursion itself
  (hardware-owned: SMU die-temp throttle + SuperIO/EC fan fallback).

## 1. The question to resolve

The 06-09 freeze (control loop + system + HWiNFO/LHM all stopped updating under
full 32-thread saturation) **does not reproduce today** with a y-cruncher run and
the shipped `BelowNormal` controller. Per the Layer-0 brief, the freeze therefore
was *not* a deterministic result of max load — it required an additional
operator-induced starvation. **Until the stall can be produced on command, every
watchdog timeout and priority threshold in the Layer-0 plan is a guess.** This
protocol finds the *minimal* condition that stalls the loop, sweeping the suspects
the brief names: the load's priority, its affinity, oversubscription — against the
controller held at `BelowNormal`.

## 2. Stall detector — how we know it reproduced

A stimulus is useless without an objective "did it stall?" signal. The loop's own
structured log supplies one:
`release/runtime/logs/svg_mb_control_events.jsonl`. The shipped tick is
`poll_tick_ms = 250`, so a healthy loop advances `tick_count` at **~4 ticks/s** and
emits `control_loop.write_applied` on setpoint deltas.

Metrics (read over the window the load runs):

1. **Effective tick cadence** — `Δtick_count / Δt` over a sliding window. Healthy
   ≈ 4/s.
2. **Max inter-event gap** — longest wall-clock gap between successive
   `control_loop.*` events.
3. **Max PWM-write gap** — longest gap between successful
   `control_loop.write_applied` events (this is the quantity the Layer-0 watchdog
   timeout is ultimately sized against — brief Phase-1 Q4).
4. **Artifact flag** — distinguish a genuine loop stall from sensor-side artifacts
   (stale/torn reads, blanked windows, `*_upsert_failed`). Prefer Tctl/Tdie over
   per-CCD per-core reads; reject impossible dT/dt or stuck registers.

**Calibration reference (real, logged):** the 2026-06-15 23:27 NDIS-hang stall
(see memory `cpu-controller-restart-ndis-hang-2026-06-15`) shows the signature in
this same log — `tick_count` advanced **22452 → 22455 in ~25 s** (≈0.12 ticks/s vs
~4/s nominal), preceded by ~24 s of `control_loop.sidecar_upsert_failed`
(Windows error 5). That incident's cause was a network-driver I/O stall, **not** CPU
starvation, so it is a detector calibration only — not a sweep result — but it
fixes what "stalled" looks like in the telemetry.

**Provisional thresholds (to be finalized from observed worst-case bounds, not
asserted here):** flag *degraded* when sustained cadence falls below ~1 tick/s
(≥ ~4× the 250 ms tick) for ≥ 2 s; flag *stall* when no successful PWM write occurs
for a watchdog-relevant window (provisionally 5–10 s). The sweep reports the actual
bounds; Phase 2 sets the final numbers.

## 3. The instrument

`tools/cpu_synth_load.cpp` → `cpu-synth-load.exe` (default-OFF flag
`SVG_MB_CONTROL_BUILD_SYNTH_LOAD`; read-only: FP register math only, no MSR/PCI/fan
writes; the scheduling knobs touch only this process). AVX2 — a deliberately safer,
lower-power proxy than a y-cruncher VT3 / AVX-512 power-virus (§5).

Knobs added for this protocol:

- `--priority below|normal|above|high` — process priority class
  (`SetPriorityClass`). Default: inherit the launcher. `realtime` is intentionally
  **not** accepted (it can preempt the driver IOCTL path and critical system
  threads, and hard-lock the box).
- `--pin` — pin worker *t* to logical CPU (*t* mod CPU count), so every core
  carries a busy thread (deterministic full occupancy; no idle slice for the
  controller to land on).
- `--oversubscribe K` — set threads = K × logical CPUs (e.g. `K=2` → 64 threads on
  32 cores → two ready threads per core, forcing time-slicing). Ignored if
  `--threads` is given.

Build (OFF by default):

```
cmake -DSVG_MB_CONTROL_BUILD_SYNTH_LOAD=ON --preset x64-release
cmake --build build/x64-release --target cpu_synth_load
```

The startup banner echoes the effective `threads / cores (ratio) / priority / pin`
for a self-documenting capture record.

## 4. Sweep — one variable at a time

Controller held at shipped `BelowNormal` throughout. Each cell: bounded
`--seconds` (suggest 120), record the load banner timestamps + the §2 detector
metrics over the overlapping `events.jsonl` window, cooldown between cells. Stop at
the first cell that crosses the *stall* threshold — that is the minimal condition.

| Cell | Command (32-core box) | Tests |
|---|---|---|
| Baseline | `--threads 32 --seconds 120` | confirms NON-repro (matches "not freezing now") |
| A1 | `--threads 32 --priority above --seconds 120` | load priority |
| A2 | `--threads 32 --priority high --seconds 120` | load priority (ceiling) |
| B | `--threads 32 --priority <A-knee> --pin --seconds 120` | + per-core affinity |
| C1 | `--oversubscribe 2 --priority <op> --pin --seconds 120` | + 2× oversubscription |
| C2 | `--oversubscribe 4 --priority <op> --pin --seconds 120` | + 4× oversubscription |

`<A-knee>` = the priority at which axis A first shows degradation (or `normal` if
none); `<op>` = the operative priority carried forward. A plain all-core load at
`normal` already does not reproduce (Windows still time-slices a `BelowNormal`
thread *some*), so the discriminator is expected on the priority/affinity axes.

## 5. Falsifiable escalation

If the most aggressive cell (`--priority high --pin --oversubscribe 4`) does **not**
cross the stall threshold across repeats, that is evidence the 06-09 mechanism
included the **thermal SMU clock-collapse** (the unquantified ~107 °C excursion),
not scheduling alone. Escalate to a **first-party AVX-512 power-matched synthetic
load** (a separate default-OFF tool — *not* y-cruncher, so the "can't use
y-cruncher" constraint does not block the fallback), operator present, SMU throttle
as backstop. AVX2 first proves the cheap hypothesis or earns the expensive one.

## 6. Safety envelope

Default-OFF build; bounded `--seconds`; **operator present** for every elevated/
pinned cell. `high` is the aggressive ceiling (realtime unavailable by design). The
backstops are hardware and already-verified: the SMU die-temp throttle, the
SuperIO/EC fan fallback (SuperIO holds the last host-written PWM), and the watchdog
task (`svg-mb-control-task-runner.exe --watchdog-run`) that restarts the supervisor
on a graceful stop — observed working during the 06-15 incident. The controller is
**not** modified: no `control.json` change, no FEAT code, `BelowNormal` preserved.

## 7. Output

A short neutral findings record (in the register of
`docs/cpu-power-anticipation-gate2-characterization-2026-06-15.md`) reporting: the
minimal reproduction condition, the worst-case bounds (longest tick gap, longest
PWM-write gap, whether `thermal_pressure` was mid-ramp at stall onset), and the
artifact-vs-genuine-stall split. Those bounds size the Layer-0 watchdog timeout and
priority scheme in Phase 2 — they are not decided here.

## 8. References

- `Suggestions/claude-brief-loop-survival-layer0-2026-06-15.md` — Layer-0 brief
  (Phase-1 reproduction gap; the suspect variables; reuse `control_thread_priority.h`).
- `docs/cpu-thermal-emergency-clamp-plan-2026-06-15.md` — the shelved clamp + the
  starvation finding.
- `docs/cpu-power-anticipation-gate2-characterization-2026-06-15.md` — neutral
  measurement-record register and the saturation telemetry-artifact precedent.
- `tools/cpu_synth_load.cpp` — the instrument; `CMakeLists.txt`
  `SVG_MB_CONTROL_BUILD_SYNTH_LOAD`.
- Memory `cpu-controller-restart-ndis-hang-2026-06-15` — the detector calibration
  example.
