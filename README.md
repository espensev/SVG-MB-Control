# SVG-MB-Control

> **⚠️ RETIRED — no longer in active duty.**
> Active development and operation of motherboard telemetry and fan control have
> moved to **SQ-control**, which now owns this responsibility. This repo's
> runtime is expected to be **off** by design — that is intentional, not a
> fault. Keep this repo for historical reference only.

[![Windows CI](https://github.com/espensev/SVG-MB-Control/actions/workflows/ci-windows.yml/badge.svg)](https://github.com/espensev/SVG-MB-Control/actions/workflows/ci-windows.yml)

`SVG-MB-Control` is the standalone runtime repo for motherboard telemetry and
fan control. This repo owns the executable, packaged configs, runtime state,
vendored dependencies, and release artifacts. It does not depend on sibling
repos at runtime.

## Quick Start

Use these as the default entry points:

```powershell
.\scripts\Test-LocalCI.ps1 -KeepBuildDir
.\build-release.ps1
```

Packaged operator commands run from `release\` after a release build:

```powershell
cd .\release
.\svg-mb-control.exe --status
.\svg-mb-control.exe --health --json
.\svg-mb-control.exe --show-config
```

For runtime pause/resume windows:

```powershell
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Status
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -For 45m -EvidenceLog
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Resume
```

Start with [docs/README.md](docs/README.md) for the local documentation map.
Use [docs/features/README.md](docs/features/README.md) before adding new
features or changing runtime behavior.

## Scope

Implemented here:

- `svg-mb-control.exe` as the only runtime executable
- direct AMD CPU telemetry
- optional direct NVIDIA telemetry through the vendored `gpu_telemetry` slice
- direct fan reads, writes, and restore through vendored `SVG-MB-SIO`
- direct `one-shot`, `read-loop`, `write-once`, `control-loop`, and
  foreground `evidence-log`
- product-owned runtime files under `runtime\`
- hermetic smoke coverage through simulation environment hooks

Repo-local dependencies live under `third_party\` and `resources\`.

Dependency hygiene: release builds should stay reproducible from repo-owned,
pinned inputs. `nlohmann/json` is vendored as a single-header dependency under
`third_party\nlohmann-json\`; the normal release configure path should not
download it from the network.

## Repo Boundary

- `SVG-MB-Control` owns process lifetime, config, policy, runtime state, and
  recovery behavior.
- `third_party\SVG-MB-SIO` is the low-level Super I/O backend.
- `third_party\nvapi-controller` contributes the vendored GPU telemetry slice.
- Legacy bridge executables are not part of the runtime contract in this repo.

## Internal Organization

The repo is standalone and the source tree is split by responsibility:

```text
src/
  app/        main, CLI parsing, mode dispatch, diagnostics
  control/    loop orchestration, channel evaluator, status model
  runtime/    runtime store, CSV logger, event log, JSON IO,
              runtime write policy, read/write mode entry points
  hardware/   AMD, GPU, SIO fan backends
  platform/   Windows timer, process metrics, HANDLE wrappers
  policy/     curves, blending, curve shape
  analyze/    runtime-log ingest, pruning, reporting
```

`svg_mb_control_core` is the static library target for non-app implementation
code. `svg-mb-control.exe` is a thin executable wrapper around it, so C++ unit
tests can exercise core behavior without launching the full executable for
every behavior check.

See `docs\STRUCTURE_AND_STABILITY.md` for the longer plan.

## Build

Preferred release build:

```powershell
.\build-release.ps1
```

Useful options:

- `-KeepBuildDir` keeps `build\` after a successful release build
- `-SkipTests` skips both test lanes (the C++ CTest lane and
  `python -m unittest discover tests -v`)
- `-NoStopProcesses` skips the pre-build stop of running `svg-mb-control`
  processes
- `-NoPublish` builds and tests without updating `release\` or creating an
  archive

Local CI-equivalent validation that does not touch a live controller:

```powershell
.\scripts\Test-LocalCI.ps1 -KeepBuildDir
```

Manual CMake build:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
```

Release-script outputs:

- `release\svg-mb-control.exe`
- `release\svg-mb-control-task-runner.exe`
- `release\control.json`
- `release\config\machines\snd-desk.cooling.policy.json`
- `release\runtime_policy_write_live.json`
- `release\Install-SVG-MB-ControlCommon.ps1`
- `release\Install-SVG-MB-ControlShortcut.ps1`
- `release\Install-SVG-MB-ControlScheduledTask.ps1`
- `release\Install-SVG-MB-ControlWatchdogScheduledTask.ps1`
- `release\Set-SVG-MB-ControlRuntimeWindow.ps1`
- `release\scripts\Compare-CpuTemps.ps1`
- `release\scripts\Install-CpuTempBaselineTask.ps1`
- `release\scripts\analyze_cpu_temp_power.py`
- `release\resources\pawnio\AMDFamily17.bin` (vendored from PawnIO.Modules
  release 0.2.6; provenance and SHA-256 in
  `third_party\pawnio\README.md`)
- `release\resources\pawnio\LpcIO.bin`
- `release\build-info.json`
- `release\VERSION_TABLE.json`
- `release\archive\svg-mb-control-<timestamp>.zip`

Publishing preserves existing `release\runtime\` state/logs and
`release\archive\`; runtime files are not included in release archives. If the
build script stops a controller that was running from `release\`, cleanup starts
the packaged controller again through `release\control.json`.

## Run

Zero-arg packaged launch:

```powershell
cd .\release
.\svg-mb-control.exe
```

The packaged `control.json` sets `default_mode` to `control-loop`, so a plain
launch starts normal fan control under a hidden supervisor, returns the shell
prompt, and writes runtime output under `release\runtime`. The supervisor
restarts the worker after an unexpected non-zero exit; config/startup failures
are still reported immediately.

Operator commands:

```powershell
cd .\release
.\svg-mb-control.exe --start
.\svg-mb-control.exe --status
.\svg-mb-control.exe --health --json
.\svg-mb-control.exe --show-config
.\svg-mb-control.exe --show-config --json
.\svg-mb-control.exe --show-config --profile snd-desk-composed --json
.\svg-mb-control.exe --stop
.\svg-mb-control.exe --restart
.\svg-mb-control.exe --reset-breakers
.\svg-mb-control.exe --reset-breakers --reset-breaker-channel 4
.\svg-mb-control.exe --set-profile snd-desk-composed
```

FEAT-0023 profile selection: the controller resolves its profile at startup by
precedence `--config` > `--profile <name>` / `SVG_MB_PROFILE` > machine identity
(`GetComputerNameW` plus an optional `runtime\machine_id.txt`) > the built-in
default, where a named profile is `config\profiles\<name>.json`. `--set-profile
<name>` switches the active profile on a running controller: the supervisor
validates the candidate and gracefully cycles the worker into it (fans revert to
BIOS SmartFan auto during the brief restart gap), with auto-revert to the
last-known-good profile if the new profile fails to start. The active profile is
recorded in `runtime\control_supervisor.json` and in
`supervisor.profile_applied` / `_rejected` / `_reverted` events.

Minimal local profile UI:

```powershell
# From the repo root. Requires a Rust toolchain in PATH.
.\scripts\Start-ProfileSwitchUi.ps1
```

The Rust helper binds to `127.0.0.1:8766`, lists profile files from the same
profile catalog locations, shows `--health` / `--status`, and applies a profile
only when the operator clicks Apply. Apply uses the existing
`svg-mb-control.exe --set-profile <name>` path; the helper does not add a new
runtime request file or switch protocol.

`--show-config` prints an operator-facing summary of the loaded config:
source path, profile source/name when selected, schema version, default mode,
loop cadence, write timing, health/safety thresholds, low-band global state, and
per-channel controller kind, PID parameters when selected, blend, source-aware
CPU guard, rate limits, smoothing, boost stages, and curve endpoints. It does not
require the controller to be running and reads the same config the worker would.
`--show-config --json` emits the same fields as a structured JSON document for
tooling. `--profile <name>` loads
`config\profiles\<name>.json`; an explicit `--config <path>` wins over
`--profile`, and `SVG_MB_PROFILE=<name>` is used when no flag is given.

`--stop` asks the running loop to shut down through `release\runtime`; it does
not hard-kill the controller. The status command prints the active worker PID,
mode, status detail, runtime home, log paths, and PawnIO hardware-access state
for the AMD/SMN read path and Super I/O write path. `--health --json` returns a
machine-readable health state: `healthy`, `degraded`, `stale`, `stopped`, or
`failed`, plus the same additive hardware-access state. The hardware-access
fields are observational and do not change health exit-code mapping.
`--reset-breakers` writes a live control-loop request to clear open
per-channel write-failure breakers without restarting; omit
`--reset-breaker-channel` to reset all channels.

Recommended Windows install:

```powershell
cd .\release
.\Install-SVG-MB-ControlScheduledTask.ps1
```

This registers a scheduled task named `SVG-MB Control` for system startup and
current-user logon, runs it elevated, and starts the controller immediately. In
the release package, the task action uses
`svg-mb-control-task-runner.exe --start --config <release\control.json>`, so
Task Scheduler does not launch the console executable directly. The runner then
starts `svg-mb-control.exe` through the same supervised launch path as the
manual start command.

Optional watchdog install:

```powershell
cd .\release
.\Install-SVG-MB-ControlWatchdogScheduledTask.ps1
```

The watchdog requires and runs through
`svg-mb-control-task-runner.exe --watchdog-run`. It evaluates
`svg-mb-control.exe --health --json` at logon and every minute, does nothing
for `healthy` or `degraded`, restarts the controller for `stale` or `stopped`,
and leaves `failed` untouched for operator review.

Task manager commands:

```powershell
.\Install-SVG-MB-ControlScheduledTask.ps1 -Status
.\Install-SVG-MB-ControlScheduledTask.ps1 -Stop
.\Install-SVG-MB-ControlScheduledTask.ps1 -Restart
.\Install-SVG-MB-ControlScheduledTask.ps1 -Remove
```

Intentional stop/pause windows:

```powershell
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Status
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Status -Json
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -For 1h
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -For 45m -EvidenceLog
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Resume
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Restart
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Stop
```

`-Pause` disables the main and watchdog tasks, requests a cooperative stop, and
registers a one-shot resume task for the requested duration or `-Until` time.
`-EvidenceLog` starts read-only `evidence-log` during the bounded window and
stops it before Control resumes; it writes `svg_mb_control_evidence.*` files
instead of control-loop CSV rows. Add `-DryRun` to review the planned task and
runtime actions without changing the live system. `-Status -Json` is the
machine-readable helper/task/window state for future external coordinators such
as SQ-control; this repo remains standalone and the contract is the packaged
script/exe process boundary.

See `docs\OPERATOR_RUNTIME_WINDOWS.md` for the full operator and coordinator
contract.

Start Menu shortcut:

```powershell
cd .\release
.\Install-SVG-MB-ControlShortcut.ps1
```

The shortcut runs `svg-mb-control.exe` with no special arguments and is created
under the Windows Start Menu programs folder. Windows does not expose a reliable
supported CLI for pinning shortcuts to Start; after installing the shortcut,
open Start, find "SVG-MB Control", then right-click it and choose "Pin to
Start".

Direct one-shot snapshot to stdout:

```powershell
release\svg-mb-control.exe --mode one-shot
```

Direct read loop:

```powershell
release\svg-mb-control.exe --mode read-loop --config .\config\control.example.json
```

Direct write-once:

```powershell
release\svg-mb-control.exe --mode write-once --config .\config\control.example.json --write-channel 4 --write-pct 60 --write-hold-ms 10000
```

Focused fan response calibration:

```powershell
release\svg-mb-control.exe --mode calibrate --config .\release\control.json --calibrate-channel 2 --calibrate-sequence 52:45000,54:45000,56:45000 --calibrate-settle-window-ms 15000
```

`--calibrate-sequence` takes comma-separated `duty_pct:hold_ms` steps and
samples inside the same process that owns the temporary write. This avoids
separate status/read commands reconciling the pending-write sidecar during a
hold window.

Attached control loop for diagnostics:

```powershell
release\svg-mb-control.exe --mode control-loop --config .\release\control.json
```

Foreground evidence logger:

```powershell
release\svg-mb-control.exe --mode evidence-log --config .\release\control.json
```

`evidence-log` writes separate CSV, event, and manifest latest files under the
same runtime home. It samples wider SIO and GPU evidence outside the controller
hot path; `evidence_gpu_sample_mode` accepts `thermal-fast`, `fast`, `medium`,
`slow`, `rare`, or `full`.

When `--mode` is omitted, Control uses `default_mode` from the loaded config.
If no config sets `default_mode`, Control falls back to `one-shot`.
For long-running default modes, the no-arg launcher starts a background
supervisor and exits; pass `--mode` explicitly when you want the current
terminal to stay attached without supervision. `--start` follows the same
supervised launch path, `--stop` writes a cooperative stop request, and
`--restart` stops first and only relaunches after the previous worker reports
stopped.

Legacy bridge flags such as `--bridge-exe-path` are intentionally rejected in
this branch.

## Analyze

Ingest CSV archives, manifests, events.jsonl, and `plant_model.json` from a
runtime home into a sqlite database for offline analysis:

```powershell
release\svg-mb-control.exe analyze ingest
release\svg-mb-control.exe analyze ingest --runtime-home .\release\runtime --db .\release\runtime\svg_mb_control.db
release\svg-mb-control.exe analyze ingest --force --quiet
release\svg-mb-control.exe analyze prune --runtime-home .\release\runtime --db .\release\runtime\svg_mb_control.db --dry-run
release\svg-mb-control.exe analyze prune --runtime-home .\release\runtime --db .\release\runtime\svg_mb_control.db --retain-days 14 --apply
release\svg-mb-control.exe analyze prune --runtime-home .\release\runtime --db .\release\runtime\svg_mb_control.db --db-retain-days 30 --apply
release\svg-mb-control.exe analyze report --runtime-home .\release\runtime --db .\release\runtime\svg_mb_control.db --idle-seconds 300
release\svg-mb-control.exe analyze report --run 7 --load-threshold-c 70 --json
release\svg-mb-control.exe analyze report --run 7 --p0-mhz 4300 --json
release\svg-mb-control.exe analyze report --run 7 --idle-seconds 300 --profile combined-load --notes "ambient and subjective noise notes" --out runtime\analysis\combined-load-summary.txt --manifest-out runtime\analysis\combined-load-manifest.json
```

Behavior:

- Default `--runtime-home` is resolved from the active config (the same
  resolution as the control modes); default `--db` is
  `<runtime-home>\svg_mb_control.db`.
- The DB schema is bootstrapped on first use (schema version `13`). The schema
  defines `runs`, `tick_samples`, `tick_fan_samples`, `tick_channel_samples`,
  `events`, `plant_model_captures`, `plant_model_channels`, and
  `plant_model_steps`; `tick_samples.gpu_envelope_c` stores the derived GPU
  control envelope used by response analysis,
  `tick_channel_samples.primary_temp_source` preserves per-channel
  CPU/GPU/guard attribution for the primary curve input,
  `tick_channel_samples.low_band_stage_boost_pct` /
  `low_band_effective_boost_pct` record the per-channel low-band boost (staged
  and effective), the nullable `tick_samples.cpu_power_sample_id` /
  `cpu_power_window_ms` / `cpu_pkg_energy_delta_uj` /
  `cpu_pkg_energy_acquisition` columns carry the read-only RAPL package-energy
  evidence (FEAT-0006) from which `analyze report` derives time-weighted
  average package power, and the nullable `tick_samples.cpu_cycles_sample_id` /
  `cpu_cycles_window_ms` / `cpu_aperf_delta` / `cpu_mperf_delta` /
  `cpu_cycles_acquisition` columns carry the read-only APERF/MPERF cycle
  evidence (FEAT-0006) from which `analyze report` derives the cycle-weighted
  APERF/MPERF ratio over distinct sample-id windows — and effective frequency
  (ratio × P0) only when `--p0-mhz <mhz>` supplies the base frequency, since
  no logged field records P0. The nullable (schema v13)
  `tick_samples.cpu_aperf_delta_allcore` / `cpu_mperf_delta_allcore` /
  `cpu_cycles_window_ms_allcore` / `cpu_cycles_allcore_sample_id` /
  `cpu_cycles_allcore_cores` columns carry the all-core package effective
  frequency from an off-thread sweep (FEAT-0006) — `analyze report` derives the
  same ratio / effective frequency over distinct `cpu_cycles_allcore_sample_id`
  windows (a `cpu_cycles_allcore` block, with the contributing-core count),
  separate from the per-core block above. The nullable
  `tick_samples.gpu_power_sample_id` /
  `gpu_power_time_ms` / `gpu_power_mw` / `gpu_power_source` /
  `gpu_power_acquisition` columns carry the read-only GPU board-power evidence
  (FEAT-0020) from which `analyze report` derives mean / p50 / p90 / max over
  distinct sample-id samples — instantaneous board power, not an energy
  integral. The nullable FEAT-0021 `tick_samples.gpu_context_sample_id` /
  `gpu_context_time_ms` / `gpu_context_sample_age_ms` /
  `gpu_context_acquisition`, `gpu_util_*`, `gpu_pstate`, `gpu_clock_*`, and
  `gpu_vram_*` columns carry cached GPU workload context from which
  `analyze report` emits a `gpu_context` summary only when present.
  GPU power records automatically on the standard loop whenever NVML returns a
  reading; to also enable the comparable CPU package-energy columns there, run
  `scripts\Set-EnergyLoggingProfile.ps1 -Enable` (and `-Disable` to revert,
  `-DryRun` to preview) — it sets `SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled` and
  keeps the profile from being undone by the boot/logon energy safety-revert task.
- Runs are deduplicated by `(session_start, mode)` and by canonical
  `manifest_path`, so re-running ingest is idempotent. The live manifest and
  its rotated archive copy resolve to a single run row.
- Plant model captures are deduplicated by canonical `capture_path`.
- `--force` deletes existing matching rows before re-ingesting them.
- `analyze ingest` is offline-only; it never writes to fans or alters runtime
  policy.
- `analyze prune` scans archive CSV/manifest bundles, defaults to dry-run, and
  requires `--apply` before deleting files. It only deletes a bundle after the
  run is present in the SQLite ingest DB and the archive is older than
  `--retain-days`; running manifests are always skipped.
- Add `--db-retain-days <days>` to the same prune command to delete analyzer DB
  `runs` older than the age bound, cascade their dependent tick/fan/channel/event
  rows under SQLite foreign keys, and run one post-delete reclaim. `0` explicitly
  disables the DB-side purge; dry-run reports candidates without deleting rows.
- Runtime retention removes the matching archive manifest when it prunes an old
  archive CSV chunk. Event JSONL files rotate into `logs\archive\*.jsonl` and are
  pruned by the runtime `log_rotate_hours` / `log_retain_days` window.
- `analyze report` summarizes one ingested run (the most recent run unless
  `--run <id>` or `--session <ts>` is given). It reports idle/load/cooldown
  `p50`/`p90`/`max` for `cpu_tctl_c` and `gpu_memjn_c`/`gpu_envelope_c`,
  per-channel `setpoint_pct`/`duty_pct`/`rpm` plus max boosts, write count and
  up/down setpoint reversals, primary temperature source counts, the response
  delay from the first
  `--load-threshold-c` crossing (default `75` C) to the first controlled-channel
  setpoint increase above its idle baseline, and authority/write/restore failure
  counts. The idle band is the ticks whose elapsed time is below
  `--idle-seconds` (default `300`, matching the evaluation passes); percentiles
  use nearest-rank on sorted ascending values where `p100` is the maximum.
  It also compares manifest-declared, archive-ingested, and latest-mirror CSV
  row counts when the runtime manifest names `artifacts.csv_latest.path`.
  Running mismatches are reported as
  `running_csv_manifest_consistency_warning`; closed-run mismatches are reported
  as `closed_csv_manifest_consistency_suspect_evidence`. `--json` emits the
  same metrics as a JSON object.
- `analyze report --out <path>` writes the text or JSON report to a file. For
  text reports, a sibling `*.decision.md` record is written automatically unless
  `--no-decision-record` is set. Use `--decision-record-out <path>` or
  `--decision-record-out auto` to override the path.
- `analyze report --manifest-out <path>` writes an analysis manifest with run
  identity, profile/notes/decision context, source artifact hashes, generated
  output hashes, response metrics, channel attribution counts, event
  severity/error-code counts, and diagnostic flags for no-response,
  slow-response, hot-but-low-setpoint, CSV evidence consistency, authority,
  write, and restore issues.
  `analyze report` is read-only; it never writes to fans, the runtime, or the
  database.

Post-run response-test summaries and decision records:

```powershell
release\svg-mb-control.exe analyze ingest --runtime-home .\release\runtime --db .\release\runtime\svg_mb_control.db
release\svg-mb-control.exe analyze report `
  --runtime-home .\release\runtime `
  --db .\release\runtime\svg_mb_control.db `
  --idle-seconds 300 `
  --profile combined-load `
  --notes "ambient and subjective noise notes" `
  --out runtime\analysis\combined-load-summary.txt `
  --manifest-out runtime\analysis\combined-load-manifest.json
```

The generated decision record includes run identity, artifact hashes, response
metrics, channel attribution counts, event counts, and automatic flags for
hot-but-low/no-response runs. `scripts\analyze_control_run.py` is a thin
convenience wrapper for archived captures that have not been ingested yet: it
shells the in-repo `svg-mb-control.exe` to `analyze ingest --csv` into a
temporary database and forwards native `analyze report` output (text or JSON),
so all analysis is native. Prefer native `analyze ingest` plus `analyze
report` for new work.

CPU temperature trend and power-normalized comparison:

```powershell
.\scripts\Compare-CpuTemps.ps1 -Label stock-preoc -AmbientC 21
python .\scripts\analyze_cpu_temp_power.py `
  --runtime-home .\release\runtime `
  --machine-policy .\config\machines\snd-desk.cooling.policy.json `
  --ambient-c 21 `
  --out .\release\runtime\analysis\cpu-temp-power-latest.md `
  --json-out .\release\runtime\analysis\cpu-temp-power-latest.json `
  --window-csv-out .\release\runtime\analysis\cpu-temp-power-latest-windows.csv
```

Use `Compare-CpuTemps.ps1` for continuity with the long busy-band baseline. Use
`analyze_cpu_temp_power.py` when CPU package-energy fields are present, so CPU
temperature is compared against actual package watts with GPU-confound and
policy-marked radiator-response context. It also reports per-CCD Tdie (the
`CCD2-CCD1 C` balance between the frequency and V-cache dies), parsed from the
existing `amd_sensor_summary` column with no schema change.

To detect BIOS/Curve-Optimizer/PBO changes from telemetry alone (no operator
annotation), `scripts\cpu_config_fingerprint.py` segments runs into auto-labeled
config regimes from config-pure dimensions; effective-MHz/W and Vcore dimensions
fill in after a `CPU_CYCLES_MODE=enabled` capture and the SVI probe
(`docs\cpu-cycles-capture-and-vcore-probe-plan-2026-06-21.md`).

Local eval dashboard:

```powershell
.\scripts\Start-EvalDashboard.ps1 -Open
```

This serves the dashboard assets and opens
`http://127.0.0.1:8765/tools/eval_dashboard/`. The dashboard auto-loads the
recent tail of the packaged live CSV mirror from
`release\runtime\logs\svg_mb_control_output.csv` when available, so long-running
logs do not freeze the browser. It can also load selected control-loop CSVs,
analyzer JSON summaries, and optional events JSONL files in the browser. The
first response watch line is CPU/Tctl at `65 C` by default, separate from the
GPU envelope limit, so below-70 C steady-state response is visible instead of
hidden behind a high thermal ceiling. Use `-RuntimeHome <path>` when inspecting
a run outside `release\runtime`; the health endpoint uses the same runtime home
and reports file freshness for the health, status, live CSV, and event sidecars.
While served over HTTP, the dashboard uses that freshness metadata to refresh
the live CSV/events view only when the files change; choosing a manual CSV or
events file disables live auto-refresh for that page session.

## Config

Canonical config files:

- `config\control.example.json` for repo-local editing
- `config\control.release.json` for packaging into `release\control.json`
- `config\runtime_policy_write_live.json` for the packaged live-write policy
- `config\machines\snd-desk.cooling.policy.json` for the machine-specific fan
  topology, idle/low-load pressure strategy, and cooling-policy data behind the
  shipped profile

Common fields:

- `default_mode`
- `snapshot_path`
- `runtime_policy_path`
- `runtime_home_path`
- `poll_ms`
- `log_rotate_hours`
- `log_retain_days`
- `evidence_gpu_sample_mode`
- `baseline_freshness_ceiling_ms`
- `restore_timeout_ms`
- `control_loop`

Field notes:

- `snapshot_path` is optional. `read-loop` always writes
  `runtime\current_state.json` and mirrors the same payload to `snapshot_path`
  when configured.
- `runtime_policy_path` is read locally by direct write and control flows.
- The shipped live policy controls airflow lanes `0,1,2,3,4,5`.
  Machine-specific fan topology and cooling intent live in
  `docs\COOLING_STRATEGY.md` and
  `config\machines\snd-desk.cooling.policy.json`.
- The packaged control loop uses GPU envelope curves plus per-channel
  `cpu_override_curve` overlays, commanding the higher duty so Cinebench plus
  max CUDA load has a CPU response path without raising idle floors.
- The SND-DESK release config keeps positive pressure with lower intake hard
  minima plus low/medium curve points on channels `2`, `3`, and `4`, rather
  than holding the old `60% / 56% / 31%` intake floor at idle.
- Response smoothing and bounded decay latching are applied before rate
  limiting so the loop emits intermediate PWM steps without chasing small
  high-temperature dips. The radiator Noctua lanes use staggered CPU overlay
  points plus a slow thermal-pressure boost for sustained high heat.
- Runtime status and CSV rows include per-channel response attribution so traces
  can distinguish primary curve demand, CPU override, thermal pressure, first
  writes, setpoint deltas, and authority reasserts.
- Runtime events include normalized `severity` and `error_code` fields for
  response-test triage and offline ingest.
- Channels `1`, `4`, and `5` also include a small CPU low/mid soak lane. It
  starts at `72 C`, fills by `82 C`, releases at `68 C`, and is capped at
  `0.3%` to give sustained
  common CPU work a slow airflow nudge without lowering the fast CPU override.
- The packaged live runtime policy keeps Channel `6` blocked.
- The packaged control loop currently uses a `250 ms` tick and `250 ms` write
  cooldown with sub-one-percent step caps so normal movement can still be
  written as small intermediate PWM steps. Runtime status and CSV logs include
  process CPU and memory fields for watching the cost of this profile.
- `log_rotate_hours` controls CSV chunk rotation under `runtime\logs\archive\`.
- `log_retain_days` controls archive pruning for rotated CSV chunks.
- `evidence_gpu_sample_mode` controls the GPU tier used only by foreground
  `evidence-log`; normal read/control loops keep using the thermal-fast GPU
  sample.
- Legacy bridge-era config keys such as `bridge_exe_path`,
  `bench_runtime_policy_path`, `logger_service_duration_ms`, and the old child
  restart / snapshot retry fields are rejected during config load.
- `config\control.example.json` is rooted at `config\`.
- `release\control.json` is copied to the release root, so its relative paths
  are root-relative instead of `config\`-relative.

## Runtime Home

Runtime-home resolution precedence:

1. `runtime_home_path` from the loaded config
2. `runtime\` next to `svg-mb-control.exe`
3. `runtime\` under the current working directory

Control writes:

- `current_state.json`
- `control_runtime.json`
- `control_supervisor.json`
- `control_health.json`
- `pending_writes.json`
- `stop.request.json`
- `circuit_breaker_reset.request.json`
- `logs\svg_mb_control_output.csv`
- `logs\svg_mb_control_events.jsonl`
- `logs\archive\svg_mb_control_events_<timestamp>.jsonl`
- `logs\archive\svg_mb_control_<mode>_<timestamp>.csv`
- `logs\svg_mb_control_evidence.csv`
- `logs\svg_mb_control_evidence_events.jsonl`
- `logs\archive\svg_mb_control_evidence_events_<timestamp>.jsonl`
- `logs\svg_mb_control_evidence_manifest.json`
- `svg-mb-control.supervisor.stdout.log`
- `svg-mb-control.supervisor.stderr.log`
- `svg-mb-control.worker.stdout.log`
- `svg-mb-control.worker.stderr.log`

`current_state.json`, `control_runtime.json`, and `pending_writes.json` remain
the authoritative live state and recovery plane. `control_runtime.json` also
publishes the active worker `process_id`, `log_csv_path`, `log_manifest_path`,
`event_log_path`, and `last_successful_restore_time` for the active mode.
`control_supervisor.json` publishes supervisor PID, worker restart count, and
last worker exit code/time; `control_health.json` records the last `--health`
assessment. `--health` / `--status` merge these sidecars.

See `docs\RUNTIME_HOME.md` for field definitions.

## Tests

Run:

```powershell
python -m unittest discover tests -v
```

The smoke suite is direct-only. It launches the real executable and uses
simulation environment hooks for hermetic AMD and fan telemetry.

The release build (`build-release.ps1`) and `Test-LocalCI.ps1` also run a C++
CTest lane of the native unit-test executables registered under `BUILD_TESTING`
in `CMakeLists.txt` before this Python lane. Run directly on a clean checkout,
`python -m unittest discover tests -v` first builds the executable; a missing or
failed build yields skipped tests rather than failures, so use `Test-LocalCI.ps1`
when a green result must mean tested. The Python lane also includes
`tests\test_feature_specs.py`, which checks feature-spec registry,
traceability, promotion-gate, and verification-log consistency.

## Documentation

The local docs index is [docs/README.md](docs/README.md).

Current authority surfaces:

- [docs/STRUCTURE_AND_STABILITY.md](docs/STRUCTURE_AND_STABILITY.md) for module
  ownership and remaining structural polish.
- [docs/BUILD_TARGETS_AND_DEPENDENCIES.md](docs/BUILD_TARGETS_AND_DEPENDENCIES.md)
  for executables, scheduled-task processes, and vendored dependencies.
- [docs/RUNTIME_HOME.md](docs/RUNTIME_HOME.md) for runtime sidecars, status,
  health, logs, manifests, and archive retention.
- [docs/CONTROL_LOOP.md](docs/CONTROL_LOOP.md),
  [docs/READ_LOOP.md](docs/READ_LOOP.md), and
  [docs/WRITE_ORCHESTRATION.md](docs/WRITE_ORCHESTRATION.md) for mode-specific
  runtime behavior.
- [docs/CONTROL_PIPELINE_MATH.md](docs/CONTROL_PIPELINE_MATH.md) for the
  maintained numerical reference.
- [docs/COOLING_STRATEGY.md](docs/COOLING_STRATEGY.md) and
  [config/machines/snd-desk.cooling.policy.json](config/machines/snd-desk.cooling.policy.json)
  for shipped cooling policy.
- [docs/features/README.md](docs/features/README.md),
  [docs/TRACEABILITY.md](docs/TRACEABILITY.md), and
  [docs/FEATURE_VERIFICATION_CHECKLIST.md](docs/FEATURE_VERIFICATION_CHECKLIST.md)
  for feature intake and verification.

Controller tuning work starts from [docs/MEASUREMENT_GATE.md](docs/MEASUREMENT_GATE.md),
[docs/RUNTIME_LOGGING_AND_EVALUATION.md](docs/RUNTIME_LOGGING_AND_EVALUATION.md),
and [docs/response-evaluation-tuning-plan.md](docs/response-evaluation-tuning-plan.md).
The current shipped configs assert a `250 ms` control/write profile; lowering
cadence, enabling an adaptive floor below that profile, adding live channels, or
broader controller strategy changes still require fresh measurement evidence
before changing defaults.

Point-in-time discovery, review, and completed implementation records live under
`docs/archive/` unless a current doc links them as active context.
