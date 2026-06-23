# FEAT-0020 live-flip validation results — 2026-06-18

**Project:** svg-mb-control
**Status:** Results record (live-flip M-evidence for FEAT-0020 / REQ-PWRLOG-04)
**Companion:** `docs/features/FEAT-0020-standard-control-loop-power-logging.md`,
`docs/power-logging-flip-plan-2026-06-18.md` (D-PWRLOG-1),
`docs/archive/implemented-plans/feat-0020-power-logging-implementation-plan-2026-06-18.md`

This records the live-runtime measurement (`M`) evidence captured when the
FEAT-0020 standard power-logging profile was deployed and enabled on the live
controller on 2026-06-18, under explicit live-runtime authorization. Only derived
numbers are recorded here; raw runtime CSV captures are not committed.

## 1. What was done (sequence)

1. **PR** opened: [#20](https://github.com/espensev/SVG-MB-Control/pull/20).
2. **Deploy (`B`):** `scripts/Build-Release.ps1 -SkipTests -KeepBuildDir` published
   commit `1ea44c7` (main exe SHA256 `4B304CBD…`). The prior live build remained
   archived for rollback (`release/archive/svg-mb-control-20260616-0609.zip`). The
   build suspended the watchdog (sentinel) before the swap and restarted the worker
   tree on completion.
3. **Enable (`M`):** `scripts/Set-EnergyLoggingProfile.ps1 -Enable` disabled the
   `SVG-MB Energy Safety Revert` task (→ `Disabled`), set User
   `SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled` (cycles left `disabled`), and restarted
   the worker tree (supervisor + foreground re-created 14:40:35).

The GPU board-power read is unconditional (piggybacked on the existing per-tick
`ThermalFast` temperature sample); the deploy alone makes it live. The CPU RAPL
package-energy read is env-gated and required the enable step.

## 2. Loop-timing evidence (REQ-PWRLOG-04 — does the added read move the 250 ms baseline?)

Tick period is 250 ms (`loop_intended_interval_ms`). `loop_work_duration_ms` is the
sensitive column (the fixed-start sleep masks sub-ms cost in `loop_slip_ms`).

| Window | n | mean | p50 | p90 | p99 | max | overruns |
|---|---|---|---|---|---|---|---|
| Pre-flip baseline (old build 06-16, energy off, last 1200) | 1200 | 3.03 | 1.66 | 4.10 | 21.44 | 24.67 | 0 |
| Post-deploy (new build, GPU read ON, energy OFF) | 296 | 4.56 | 3.43 | 8.96 | 20.70 | 25.91 | 0 |
| Post-flip steady (new build, energy ON, excl. first 50 ticks) | 554 | 3.45 | 2.57 | 6.94 | 12.98 | 34.9 | 0 |

All times in ms. Against a 250 ms period the worst sustained tick uses ~14 % of the
period; steady-state mean is unchanged vs the old build (3.0–3.5 ms).

### 2.1 The added per-tick NVML read does NOT stall under GPU load

The enabled session (996 rows) spanned GPU board power 78 W → 560 W (mean 368 W,
648 ticks at ≥ 350 W). `loop_work_duration_ms` bucketed by GPU load:

| GPU load bucket | n | mean | max | ticks > 50 ms |
|---|---|---|---|---|
| idle < 150 W | 322 | 14.20 | 2455.2 | 3 |
| mid 150–350 W | 26 | 4.19 | 11.0 | 0 |
| **load ≥ 350 W** | **648** | **3.15** | **14.6** | **0** |

Every slow tick (2455, 279, 117, 43, 40, 39, 37, 35 ms) occurred at GPU **idle**
(78–86 W). Under heavy GPU load the per-tick `nvmlDeviceGetPowerUsage` read was the
fastest and cleanest (max 14.6 ms, zero ticks > 50 ms). This refutes the
"per-tick NVML read stalls on the hot path under load" risk.

### 2.2 The multi-hundred-ms/second spikes are pre-existing and environmental

The old build (no hot-path NVML call, energy off) over its full 38 991-tick session
(`…115446.csv`) had `loop_work_duration_ms` mean 3.50, p50 1.76, p99 20.70, **max
3274 ms**, with 28 ticks > 100 ms, 9 > 250 ms, and **5 > 1000 ms**. So sub-second
to ~3 s tick-work spikes are a machine-level background phenomenon present without
FEAT-0020. The enabled session's single burst (ticks 29–34, ~7 s after restart,
GPU idle) is within
that pre-existing envelope and did not recur across the following ~960 ticks, and did
not trigger a watchdog recycle (worker PIDs stable since 14:40:35).

## 3. Power-data evidence (REQ-PWRLOG-01/02/03/05)

Live `analyze ingest` over the runtime home ingested 55 runs / 1 919 930 tick
samples, migrating older v9/v10 archives to schema v11; the old-build session
(run 316) and both new sessions (317 energy-off, 318 energy-on) ingested cleanly —
**old archives still ingest**.

`analyze report --json` on the enabled session (run 318, 1659 ticks):

- **`gpu_power`** (instantaneous mW; mean/percentile, not an energy integral):
  `avg_mw` 334 941; `mw` p50 487 189, p90 524 199, max 559 605; `acquisition_counts`
  `nvml` 1658, `unavailable` 1; `sample_count` 1659.
- **`package_power`** (CPU, time-weighted Σenergy/Σwindow): `avg_watts` 86.74
  (= 36 289.06 J / 418.36 s); `watts` p50 81.41, p90 111.89, max 178.92;
  `acquisition_counts` `quarantine` 1659; `window_count` 415. Single derivation —
  no second CPU watts column.

Live CSV markers: `gpu_power_acquisition`/`gpu_power_source` = `nvml`,
`gpu_power_mw` nonempty (no false zero — one leading `unavailable` tick before NVML
settled); `cpu_pkg_energy_acquisition` = `quarantine` (never live `validated`);
`cpu_power_sample_id` populating.

## 4. Control identity (REQ-PWRLOG-03)

`channel0_response_source` was `primary_curve` for 100 % of the idle windows
(296 + 144 rows). Over the full 4264-row enabled session — during which the GPU
loaded to 560 W — the source was `primary_curve` plus the existing
**GPU-temperature**-driven boosts (`gpu_airflow`, `midband_pressure`), never any
power-derived term: `primary_curve` 1336, `+midband_pressure+gpu_airflow` 2058,
`+midband_pressure` 611, `+gpu_airflow` 259. The control law responded to GPU
**heat** (temperature) via its pre-existing terms while `gpu_power_mw` was merely
logged — confirming power is observed, not consumed. `power_anticipation.h` stays
unwired by `src/control`/`src/runtime`.

## 5. Verdict

Gate 6 / REQ-PWRLOG-04 passes: the added per-tick GPU NVML read and the enabled CPU
RAPL energy read do not move the 250 ms loop-timing baseline (steady-state mean
unchanged; under-load max 14.6 ms ≪ 250 ms; no overruns), the residual spikes are
environmental and pre-existing, GPU and CPU package power are recorded and summarized
correctly, old archives still ingest under v11, and control identity is preserved.

## 6. Reversibility

To restore the FEAT-0006 boot-OFF safety net: `scripts/Set-EnergyLoggingProfile.ps1
-Disable` (re-enables the `SVG-MB Energy Safety Revert` task, sets the env back to
`disabled`, restarts the worker). To roll back the binary: republish
`release/archive/svg-mb-control-20260616-0609.zip` or rebuild the prior commit.
