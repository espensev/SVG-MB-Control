# CPU Temperature Comparison Harness

## Status

Current as of 2026-06-09. Tool: `scripts\Compare-CpuTemps.ps1`. Read-only.

## Purpose

`Compare-CpuTemps.ps1` characterizes CPU package temperature (`cpu_tctl_c`,
the AMD `Tctl/Tdie` sensor) as a function of sustained whole-system CPU load,
so the same machine can be compared across cooling/config settings over time.
It is a passive post-processor over the control-loop CSV that the running
controller already produces; it does not sample hardware itself, start/stop the
controller, or write fan duty, runtime state, or the analysis database.

It fills the one gap left by native `analyze report`: that command bins ticks by
*elapsed time* (idle = first `--idle-seconds`) and a temperature threshold, one
ingested run at a time (see `AssignBands` in
`src\analyze\analyze_report_queries.cpp`). This harness instead bins by *measured
load* and accumulates a cross-setting comparison table.

## What it reads

The harness binds three columns by name from a control-loop CSV:

- `snapshot_time` — per-row ISO timestamp, used for the dwell window.
- `cpu_tctl_c` — CPU package temperature, the measured quantity.
- `system_cpu_busy_pct` — whole-system CPU busy time (FEAT-0002), the load axis.

It also reads the CSV comment prologue for the setting identity: `git_hash`,
`config_sha256`, `build_version`, and `session_start`.

`system_cpu_busy_pct` is required. The 2026-05-28 packaged build did not emit it
(the documented "active-package header drift"); that finding is closed as of the
2026-06-09 rebuild (`git_hash=dd2c02214128`, session `2026-06-09T02:32:40`),
whose CSV header contains the five `system_cpu_*` columns. If the harness is run
against a CSV without `system_cpu_busy_pct`, it stops with an instruction to
rebuild via `scripts\Build-Release.ps1`.

## Regimes

Each sample is classified by `system_cpu_busy_pct` into one of three bands
(thresholds are `-IdleMaxPct` / `-LowMaxPct`, defaults shown):

| Regime | Definition (default) |
|---|---|
| idle | `busy_pct < 10` |
| low  | `10 <= busy_pct < 40` |
| high ("across the board") | `busy_pct >= 40` |

## Dwell gate (load-settled filter)

CPU die temperature lags load by tens of seconds, so pairing an instantaneous
load reading with an instantaneous temperature reading mislabels every ramp: at
the start of a load step the load is high while the die is still cool.

A sample counts toward a band only when the load has been continuously in that
same band, with no coverage gap larger than `-MaxGapSeconds` (default `5`), for
at least `-DwellSeconds` (default `45`) ending at that sample. The gap check
means a sensor stall or a sparse/merged archive cannot let a single post-gap
reading pass the gate vacuously. Each in-band sample that does not pass is
counted as either `ramp` (a different band appeared inside the dwell window) or
`no-dwell` (insufficient or gapped leading history); both are excluded from the
per-band percentiles. Load that oscillates across a band boundary (for example
hovering near 10%) therefore contributes to no band's percentiles, which is the
intended behavior.

The gate proves the *load* was settled, not that the *temperature* has
plateaued. Over a long sustained in-band episode the admitted samples are
dominated by the plateau, so p50/p90/max approximate steady state; for short
bursts of high load the high band can still include rising-die samples. Raise
`-DwellSeconds` if you need strict thermal equilibrium for the high regime.

Percentiles use nearest-rank on ascending-sorted values
(`index = round((p/100) * (n - 1))`, `p100` = max), the same formula as the
native `analyze report` (`src\analyze\analyze_report_data.cpp`), so a band's
p50/p90 here equals the native analyzer's for the same sample set.

## Setting identity and ambient

Each recorded run is tagged with:

- `-Label` (operator name for the setting, e.g. `shipped-250ms-baseline`),
- `-AmbientC` (required room temperature; cross-setting comparisons taken on
  different days are uninterpretable without it),
- the CSV's `git_hash` and `config_sha256`.

A config change requires a controller restart for the controller to load it,
which starts a new CSV session with a new `config_sha256`. One control-loop CSV
session therefore corresponds to one setting.

## Usage

```powershell
# Record the current session as a labeled baseline:
.\scripts\Compare-CpuTemps.ps1 -Label "shipped-250ms-baseline" -AmbientC 22.5 `
    -Notes "first session baseline, normal desktop use"

# Live-monitor the bands filling without recording anything:
.\scripts\Compare-CpuTemps.ps1 -Label live -AmbientC 22 -Watch

# Summarize a past session from an archive instead of the live mirror:
.\scripts\Compare-CpuTemps.ps1 -Label old-profile -AmbientC 21 `
    -Csv .\release\runtime\logs\archive\svg_mb_control_control-loop_<ts>.csv
```

Useful parameters: `-IdleMaxPct`, `-LowMaxPct` (band thresholds),
`-DwellSeconds` (settle requirement), `-MaxGapSeconds` (max coverage gap inside
the dwell window), `-WindowMinutes` (accumulate only the last N minutes, while
the dwell gate still uses full preceding history), `-LedgerDir` (output
location), `-Watch` / `-WatchIntervalSeconds`.

## Output

1. A console table of per-regime settled-sample count, `ramp` and `no-dwell`
   exclusion counts, achieved mean busy%, and `cpu_tctl_c` p50 / p90 / max, plus
   a count of rows dropped during parsing (blank/NaN, bad timestamp, short row).
2. An appended row per regime in the ledger CSV.
3. A regenerated Markdown `comparison.md` with two tables: a **By setting**
   rollup that aggregates every capture sharing a `label` + `config_sha256`
   (median of each capture's p50/p90 as a typical value, worst single `max`,
   and the capture count per band), and a **Recent captures** detail table
   (latest 48 captures). The rollup is the view for comparing settings; the
   detail table is the recent time series.

Default output location (gitignored, preserved across publishes):
`release\runtime\experiments\cpu-temp-comparison\`
(`ledger.csv` and `comparison.md`).

All numeric parsing and output use invariant culture, so a decimal-comma locale
does not corrupt the ledger.

## Long-term baseline (scheduled task)

`scripts\Install-CpuTempBaselineTask.ps1` registers a Windows scheduled task that
runs the harness on a fixed cadence so a multi-day baseline accumulates the same
way the controller's own logger runs. It is read-only (RunLevel Limited, no
elevation at run time) and reuses the controller's `\SVG-MB Control\` task folder.

```powershell
# Record a data point every 15 min (30-min window) under label 'stock-preoc':
.\scripts\Install-CpuTempBaselineTask.ps1 -AmbientC 22

.\scripts\Install-CpuTempBaselineTask.ps1 -Status     # state, last run, capture count
.\scripts\Install-CpuTempBaselineTask.ps1 -Remove     # when moving on to OC/UC
```

Defaults: `-Label stock-preoc`, `-IntervalMinutes 15`, `-WindowMinutes 30`,
`-DwellSeconds 45`. The task triggers at logon plus a repetition interval (up to
one year), with `StartWhenAvailable` and `MultipleInstances IgnoreNew`.

`-AmbientC` is required and is recorded fixed on every run, because the harness
cannot read room temperature. If the room temperature changes materially during
the baseline, edit `ambient_c` in the ledger or re-install with a new value.
Registering a task in the shared task folder can require an elevated PowerShell on
some systems; `-Remove` unregisters it.

## Limitations

- Passive: a regime's percentiles are populated only once that load is actually
  sustained for the dwell window. The `high` regime stays empty until a real
  sustained workload runs; this is expected, and the console flags empty bands.
- The live mirror `svg_mb_control_output.csv` holds only the active CSV chunk.
  For a session that has rotated (`log_rotate_hours`), point `-Csv` at the
  specific archive chunk to summarize the full session.
- It compares one CSV per run. It does not merge multiple archive chunks of one
  long session; pass the chunk you want.
- It is not a substitute for native `analyze report`, which covers fan
  setpoints, GPU envelope, loop timing, response delay, and write/restore
  accounting. Use both: this for temperature-vs-load across settings, the native
  analyzer for the full per-run control evaluation.
- Comparisons are only as controlled as the workload and ambient. The harness
  records ambient and the achieved mean busy% per band so an apples-to-apples
  check is possible, but it cannot correct for a different background workload
  between settings.

## Safety

Read-only under the `AGENTS.md` Live Runtime Safety rules: no controller start,
stop, restart, breaker reset, or fan write; it reads the CSV with shared access
(it does not block the live writer) and writes only to its own experiments
directory.
