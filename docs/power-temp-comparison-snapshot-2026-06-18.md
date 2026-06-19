# Power/Temperature Comparison Snapshot - 2026-06-18

**Status:** Captured
**Purpose:** preserve the first standard control-loop window where CPU package
power and GPU power are logged beside temperatures, so future temperature
comparisons can be normalized by actual power.

## Source

- Runtime source CSV:
  `release/runtime/logs/archive/svg_mb_control_control-loop_20260618_181946.csv`
- Session start: `2026-06-18T18:19:46`
- Runtime git hash in CSV header: `1ea44c758ae3`
- Runtime state at export: `healthy`, `running`, worker PID `33148`
- Export time: `2026-06-18T18:40:24`

## Saved Local Artifacts

The raw runtime CSV remains under `release/` and is intentionally not a tracked
artifact. The compact local exports are:

- `release/runtime/analysis/power-temp-windows-20260618-184019.csv`
- `release/runtime/analysis/power-temp-buckets-20260618-184019.csv`
- `release/runtime/analysis/gpu-power-temp-buckets-20260618-184019.csv`
- `release/runtime/analysis/power-temp-summary-20260618-184019.json`

`power-temp-windows-*` is one row per CPU package-energy sample window. It keeps
the comparison fields needed for later analysis: CPU package watts, CPU/GPU
temperatures, GPU board watts, CPU busy percent, fan duties, and channel
setpoints.

`power-temp-buckets-*` groups the window rows by rounded CPU Tctl temperature so
same-temperature / different-power cases are easy to find.

`gpu-power-temp-buckets-*` groups the same window rows by rounded GPU memory
junction temperature so GPU power is preserved as first-class comparison context.

## Capture Summary

- Complete control-loop rows scanned: `4910`
- Rows with CPU package power data: `4906`
- Distinct CPU package-energy windows: `1228`
- Window range: `2026-06-18T18:19:47` to `2026-06-18T18:40:19`
- CPU package acquisition marker: `quarantine` for all `1228` windows
- CPU cycles marker: intentionally disabled for this standard power snapshot
- GPU power source: NVML, captured in the same standard control-loop CSV

CPU package watts across the captured windows:

- min `35.703 W`
- p50 `61.740 W`
- p90 `75.431 W`
- max `168.288 W`
- avg `61.647 W`

CPU Tctl across the captured windows:

- min `45.469 C`
- p50 `56.031 C`
- p90 `62.750 C`
- max `72.375 C`
- avg `55.963 C`

GPU board power averaged per CPU window:

- min `57.894 W`
- p50 `527.012 W`
- p90 `599.964 W`
- max `601.490 W`
- avg `394.794 W`

## Why This Matters

Temperature alone is not a stable comparison basis. A `60 C` CPU sample at
roughly `46 W` and a `60 C` CPU sample at roughly `103 W` describe different
cooling states, even though the displayed temperature is the same. This snapshot
keeps the power context attached to the temperature rows.

Largest same-temperature CPU package-power spreads found in this capture:

| Rounded CPU Tctl | Windows | CPU W min | CPU W max | Spread | Example low-power time | Example high-power time |
|---:|---:|---:|---:|---:|---|---|
| 60 C | 40 | 46.292 | 102.541 | 56.249 | 2026-06-18T18:25:03 | 2026-06-18T18:28:46 |
| 61 C | 20 | 52.027 | 105.566 | 53.539 | 2026-06-18T18:25:26 | 2026-06-18T18:24:11 |
| 59 C | 59 | 46.397 | 97.422 | 51.025 | 2026-06-18T18:25:19 | 2026-06-18T18:24:52 |
| 55 C | 160 | 44.424 | 92.827 | 48.403 | 2026-06-18T18:25:15 | 2026-06-18T18:19:59 |
| 57 C | 143 | 48.107 | 94.762 | 46.655 | 2026-06-18T18:25:29 | 2026-06-18T18:22:37 |

GPU power also needs to travel with the thermal evidence. The GPU board-power
range reaches about `600 W`, so GPU temperature rows without watts are also
ambiguous. Same GPU memory-junction temperature can represent very different
board-power states.

Largest/highest GPU board-power buckets found in this capture:

| Rounded GPU mem junction | Windows | GPU W min | GPU W p50 | GPU W max | Spread | Avg CPU W |
|---:|---:|---:|---:|---:|---:|---:|
| 74 C | 308 | 397.846 | 599.110 | 601.490 | 203.644 | 65.627 |
| 58 C | 7 | 584.766 | 599.705 | 601.489 | 16.723 | 62.230 |
| 64 C | 8 | 433.480 | 599.903 | 601.091 | 167.611 | 59.730 |
| 62 C | 6 | 153.983 | 598.984 | 600.998 | 447.015 | 55.127 |
| 66 C | 11 | 138.182 | 527.590 | 600.895 | 462.713 | 59.888 |
| 54 C | 19 | 97.156 | 104.108 | 600.855 | 503.699 | 57.321 |
| 70 C | 87 | 157.144 | 521.142 | 600.774 | 443.630 | 64.252 |
| 72 C | 265 | 349.016 | 534.473 | 600.756 | 251.740 | 67.398 |

## Notes

- This is logging evidence, not a controller behavior change.
- CPU package power remains marked `quarantine`; live runtime does not emit
  `validated`. Promotion is a separate FEAT-0006 maintainer decision.
- CPU package watts are derived from the logged energy delta and window:
  `(cpu_pkg_energy_delta_uj / 1e6) / (cpu_power_window_ms / 1000)`.
- GPU watts are instantaneous NVML board-power samples averaged across each CPU
  package-energy window for comparison.
