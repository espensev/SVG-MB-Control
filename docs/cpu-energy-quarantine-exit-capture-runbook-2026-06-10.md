# CPU Energy Quarantine-Exit Capture Runbook — 2026-06-10

Status: **prepared, not yet executed.** Operational runbook for capturing one
*enabled-path* CPU energy (and optionally cycle) evidence session and scoring it
against the quarantine-exit Evaluation. The criteria are normative in
`docs/cpu-work-energy-acquisition-decision-2026-06-07.md` §Evaluation /
§Quarantine; this doc only operationalizes how to run a session and read the
result. It does **not** restate or change those criteria.

**Companion to:** `docs/features/FEAT-0006-cpu-work-energy-efficiency-evidence.md`
(§14), `docs/cpu-work-energy-acquisition-decision-2026-06-07.md` (§Evaluation,
§Apply order), `AGENTS.md` §Live Runtime Safety, `docs/RUNTIME_LOGGING_AND_EVALUATION.md`.

> **Promotion is multi-session.** Quarantine ends only when all Evaluation
> criteria hold across **≥ 3 independent capture sessions spanning ≥ 7 days**
> (decision §Quarantine). One run of this runbook produces **one** session's
> evidence. It does not by itself flip any `*_acquisition` marker to `validated`.

## 0. Current state (verified 2026-06-10, read-only)

- Live worker git hash `c4b6986`; the shipped exe already contains the
  energy + cycle code. The control-loop CSV header already carries the additive
  columns (`cpu_power_sample_id`, `cpu_power_window_ms`, `cpu_pkg_energy_delta_uj`,
  `cpu_pkg_energy_acquisition`, `cpu_cycles_sample_id`, `cpu_cycles_window_ms`,
  `cpu_aperf_delta`, `cpu_mperf_delta`, `cpu_cycles_acquisition`).
- Current `cpu_pkg_energy_acquisition` = `cpu_cycles_acquisition` = `disabled`
  (env vars unset). **No rebuild is required** — enabling is an env-var + restart.
- Worker is launched by scheduled task `\SVG-MB Control\SVG-MB Control`
  (`svg-mb-control-task-runner.exe --start`); a second task
  `\SVG-MB Control\SVG-MB Control Watchdog` (`--watchdog-run`) respawns the worker
  on death. The worker reads `SVG_MB_CONTROL_RAPL_ENERGY_MODE` /
  `SVG_MB_CONTROL_CPU_CYCLES_MODE` once at reader init (`amd_reader.cpp`).

## 1. Pre-session criterion-6 baseline (captured 2026-06-10, energy disabled)

From the live disabled session `svg_mb_control_output.csv`
(`2026-06-09T22:25:45 → 2026-06-10T00:24:07`, 28 268 rows @ 250 ms):

| Metric | Baseline (energy `disabled`) |
|---|---|
| `loop_slip_ms` p50 / p95 / p99 | 0.81 / **1.56** / 2.02 ms |
| `loop_slip_ms` steady max (excl. >500 ms OS outliers) | 491.76 ms |
| `loop_slip_ms` whole-run max | 4117 ms (1 OS-sleep outlier) |
| `loop_overrun = true` rows | **13** / 28 268 (~6.5/h) |
| slips > 50 ms | 13 (OS scheduler/sleep, not loop work) |

The criterion-6 gate is **no increase** vs this baseline, not absolute zero: the
disabled baseline already shows ~6.5 overrun rows/h and a tiny tail of OS-induced
slips. Recompute the same statistics on a fresh disabled window immediately
before the enabled session if hardware load conditions differ.

## 2. Capture profile (one session)

Drive the machine through, in order, on the live (energy-enabled) worker:

1. **Idle** ~5 min — establishes the idle power floor (criterion 2).
2. **Sustained all-core CPU load** ≥ 6 min — long enough to cross **at least one**
   32-bit energy wrap (~5–6 min at ~200 W; criterion 1). e.g. y-cruncher or an
   equivalent steady all-core workload. Hold a steady sub-window for the external
   cross-check (criterion 3).
3. **Cooldown** ~5 min — confirms power falls with load (criterion 2).

A CPU load is required (energy tracks CPU power, not GPU). This is a different
workload from the task-2 GPU-load capture.

## 3. External power reference (criterion 3 — must be RAPL-independent)

Criterion 3 needs an average-watts reference from a path **independent of the
RAPL energy MSR** `0xC001029B`, over a steady load sub-window, agreeing within
**±15 %**. Independence is load-bearing: a RAPL-derived reference uses the same
counter + ESU assumption, so ±15 % agreement would be near-tautological and could
not catch a wrong-encoding error.

Available sources on this machine, classified:

| Source | Path / access | RAPL-independent? | Use for criterion 3 |
|---|---|---|---|
| **AMD Ryzen Master SDK** (`AMDRyzenMasterSDKTask`) | SMU/PPT | **Yes** (SMU) | **Preferred** |
| **HWiNFO** CSV | `e:\SQ_HQ\Monitoring\hwinfo\power.CSV`, col `CPU Package Power [W]` | Only if it is the **SMU** sensor, not HWiNFO's RAPL one | Use **only after confirming** it is the SMU sensor |
| HWiNFO shared memory | live | same caveat as the CSV | same caveat |
| LibreHardwareMonitor | `http://localhost:8085/data.json` | **No** — its CPU package power is RAPL-derived | Context only; **not** valid for criterion 3 |

Record the steady-window timestamps so the control-loop energy window can be
aligned to the reference sample. This is a one-shot **manual** comparison written
into the exit note — not a runtime dependency (`AGENTS.md` §Repo Boundary).

## 4. Enable → capture → revert (operator, supervised)

> Live Runtime Safety: read-only telemetry only; never writes fan duty, MSRs, or
> CPU control. Energy mode is the production read-only path (`amd_reader.cpp`),
> not the throwaway probe. Run supervised; revert to default-off after the window.

**Enable** (PowerShell, elevated for scheduled-task control):

```powershell
[Environment]::SetEnvironmentVariable('SVG_MB_CONTROL_RAPL_ENERGY_MODE','enabled','User')
# optional, only to also exercise the cycle path (independent gate):
# [Environment]::SetEnvironmentVariable('SVG_MB_CONTROL_CPU_CYCLES_MODE','enabled','User')

# Restart the whole chain so worker + watchdog relaunch and inherit the new env.
# Stop the watchdog first so it does not respawn the worker mid-swap.
Stop-ScheduledTask  -TaskPath '\SVG-MB Control\' -TaskName 'SVG-MB Control Watchdog'
Stop-ScheduledTask  -TaskPath '\SVG-MB Control\' -TaskName 'SVG-MB Control'
Start-ScheduledTask -TaskPath '\SVG-MB Control\' -TaskName 'SVG-MB Control'
Start-ScheduledTask -TaskPath '\SVG-MB Control\' -TaskName 'SVG-MB Control Watchdog'
```

**Verify enabled** before capturing: a new session file appears under
`release\runtime\logs\`; in a data row `cpu_pkg_energy_acquisition` reads
`quarantine` (not `disabled`), and `cpu_pkg_energy_delta_uj` / `cpu_power_window_ms`
are populated on resource-window rows. (`cpu_cycles_acquisition` = `quarantine`
only if cycles were also enabled.)

**Capture** the §2 profile, recording the §3 reference window times.

**Revert to default-off** after the window (do not leave enabled until the gate
passes — decision §Disturbance mitigation §1):

```powershell
[Environment]::SetEnvironmentVariable('SVG_MB_CONTROL_RAPL_ENERGY_MODE','disabled','User')
# [Environment]::SetEnvironmentVariable('SVG_MB_CONTROL_CPU_CYCLES_MODE','disabled','User')
Stop-ScheduledTask  -TaskPath '\SVG-MB Control\' -TaskName 'SVG-MB Control Watchdog'
Stop-ScheduledTask  -TaskPath '\SVG-MB Control\' -TaskName 'SVG-MB Control'
Start-ScheduledTask -TaskPath '\SVG-MB Control\' -TaskName 'SVG-MB Control'
Start-ScheduledTask -TaskPath '\SVG-MB Control\' -TaskName 'SVG-MB Control Watchdog'
```

Confirm `cpu_pkg_energy_acquisition` is back to `disabled` in the post-revert session.

## 5. Analyze (read-only, after the session)

Identify the enabled session CSV under `release\runtime\logs\archive\` (rotated)
or `release\runtime\logs\`, then:

```powershell
python scripts\analyze_control_run.py --csv <enabled-session>.csv --format markdown
# or natively:
# .\release\svg-mb-control.exe analyze ingest --csv <enabled-session>.csv --db <tmp>.db --quiet
# .\release\svg-mb-control.exe analyze report --db <tmp>.db
```

The report derives time-weighted average package power from
`cpu_pkg_energy_delta_uj` over **distinct** `cpu_power_sample_id` windows (mirrored
250 ms rows are de-duplicated). The cycle derivation landed as analyze schema
v10 (2026-06-10): the same report emits a `cpu_cycles` block with the
cycle-weighted `ΔAPERF/ΔMPERF` ratio over distinct `cpu_cycles_sample_id`
windows, and effective MHz when `--p0-mhz <base>` is passed (no logged field
records P0) — use that for criterion 4 instead of deriving by hand.

## 6. Score this session against the Evaluation criteria

Per decision §Evaluation. This session contributes to (does not complete) the gate.

| # | Criterion | How to read it from this session |
|---|---|---|
| 1 | Counter continuity across ≥1 32-bit wrap | Energy deltas stay continuous over the ≥6 min load (no negative-after-modular, no blank-storm; implausibility guard does not fire within ~1 s windows). |
| 2 | Plausible range + load tracking | Derived avg watts ≥ 0, below the socket ceiling (proposed 400 W), near floor at idle, rises/falls with `system_cpu_busy_pct` on load on/off. Reject if pinned-constant, negative, or above ceiling. |
| 3 | ±15 % external cross-check (RAPL-independent) | RAPL-derived avg watts over the steady window vs Ryzen Master / confirmed-SMU HWiNFO within ±15 %. If only a RAPL-derived reference is available, this validates Δt/window handling but **not** ESU encoding — argue encoding separately from idle-floor + load-tracking + documented ESU. |
| 4 | Effective-frequency validity (only if cycles enabled) | `ΔAPERF/ΔMPERF × reference` lands between idle and rated boost and is stable under affinity. Cross-core/impossible values ⇒ affinity not honored ⇒ cycles stay quarantined; energy can still pass independently. |
| 5 | Fault behavior | Any unsupported/absent MSR blanks cleanly — no crash, no false zero. Confirm no `0` where a blank is expected. |
| 6 | No-disturbance vs §1 baseline | `loop_slip_ms` p95 increase ≤ ~1 baseline SD (baseline p95 1.56 ms), no `loop_overrun` rate increase beyond ~6.5/h, no rise in SMN/temperature read failures, no rise in steady-state `control_loop.authority_reasserted` (exclude startup rows). |

## 7. Record

Write a dated exit-evidence note (e.g. `docs/cpu-energy-quarantine-exit-evidence-<date>.md`)
with: the session window, ESU read, idle/load/cooldown avg watts, the §3 external
comparison (source + numbers + % delta), the §6 no-disturbance deltas, and any
criterion not met. After **≥ 3 sessions over ≥ 7 days** all pass, the maintainer
records the promotion and flips `cpu_pkg_energy_acquisition` to `validated` in a
follow-up note (and `cpu_cycles_acquisition` only if criterion 4 also passed);
then reconcile FEAT-0006 §14 / `docs/TRACEABILITY.md`. Promotion is never automatic.
