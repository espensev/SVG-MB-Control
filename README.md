# SVG-MB-Control

`SVG-MB-Control` is the standalone runtime repo for motherboard telemetry and
fan control. This repo owns the executable, packaged configs, runtime state,
vendored dependencies, and release artifacts. It does not depend on sibling
repos at runtime.

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

## Long-Term Organization Target

Keep this repo standalone, but split the source tree by responsibility as the
controller stabilizes:

```text
src/
  app/        main, CLI parsing, mode dispatch
  control/    loop orchestration, channel evaluator, status model
  runtime/    runtime store, CSV logger, event log, JSON IO
  hardware/   AMD, GPU, SIO fan backends
  platform/   Windows timer, process metrics, HANDLE wrappers
  policy/     curves, blending, rate limits, demand smoothing
```

Add a `svg_mb_control_core` static library target and keep
`svg-mb-control.exe` as a thin executable wrapper around it. That gives the
controller a stable internal API and makes C++ unit tests possible without
launching the full executable for every behavior check.

See `docs\STRUCTURE_AND_STABILITY.md` for the longer plan.

## Build

Preferred release build:

```powershell
.\build-release.ps1
```

Useful options:

- `-KeepBuildDir` keeps `build\` after a successful release build
- `-SkipTests` skips `python -m unittest discover tests -v`
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
- `release\control.json`
- `release\runtime_policy_write_live.json`
- `release\Install-SVG-MB-ControlScheduledTask.ps1`
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
.\svg-mb-control.exe --stop
.\svg-mb-control.exe --restart
.\svg-mb-control.exe --reset-breakers
.\svg-mb-control.exe --reset-breakers --reset-breaker-channel 4
```

`--show-config` prints an operator-facing summary of the loaded
`control.json`: schema version, default mode, loop cadence, write timing,
health/safety thresholds, low-band global state, and per-channel blend,
rate limits, smoothing, boost stages, and curve endpoints. It does not
require the controller to be running and reads the same config the worker
would. `--show-config --json` emits the same fields as a structured JSON
document for tooling.

`--stop` asks the running loop to shut down through `release\runtime`; it does
not hard-kill the controller. The status command prints the active worker PID,
mode, status detail, runtime home, and log paths. `--health --json` returns a
machine-readable health state: `healthy`, `degraded`, `stale`, `stopped`, or
`failed`. `--reset-breakers` writes a live control-loop request to clear open
per-channel write-failure breakers without restarting; omit
`--reset-breaker-channel` to reset all channels.

Recommended Windows install:

```powershell
cd .\release
.\Install-SVG-MB-ControlScheduledTask.ps1
```

This registers an at-logon scheduled task named `SVG-MB Control` for the current
user, runs it elevated, and starts the controller immediately. The task action
uses `svg-mb-control-task-runner.exe --start --config <release\control.json>`
when the native task runner is present, so Task Scheduler does not launch the
console executable directly. The runner then starts `svg-mb-control.exe` through
the same supervised launch path as the manual start command.

Optional watchdog install:

```powershell
cd .\release
.\Install-SVG-MB-ControlWatchdogScheduledTask.ps1
```

The watchdog runs through `svg-mb-control-task-runner.exe --watchdog-run` when
the native task runner is present. It evaluates `svg-mb-control.exe --health
--json` at logon and every minute, does nothing for `healthy` or `degraded`,
restarts the controller for `stale` or `stopped`, and leaves `failed` untouched
for operator review.

Task manager commands:

```powershell
.\Install-SVG-MB-ControlScheduledTask.ps1 -Status
.\Install-SVG-MB-ControlScheduledTask.ps1 -Stop
.\Install-SVG-MB-ControlScheduledTask.ps1 -Restart
.\Install-SVG-MB-ControlScheduledTask.ps1 -Remove
```

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
release\svg-mb-control.exe analyze report --runtime-home .\release\runtime --db .\release\runtime\svg_mb_control.db --idle-seconds 300
release\svg-mb-control.exe analyze report --run 7 --load-threshold-c 70 --json
```

Behavior:

- Default `--runtime-home` is resolved from the active config (the same
  resolution as the control modes); default `--db` is
  `<runtime-home>\svg_mb_control.db`.
- The DB schema is bootstrapped on first use (schema version `6`). The schema
  defines `runs`, `tick_samples`, `tick_fan_samples`, `tick_channel_samples`,
  `events`, `plant_model_captures`, `plant_model_channels`, and
  `plant_model_steps`; `tick_samples.gpu_envelope_c` stores the derived GPU
  control envelope used by response analysis.
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
- Runtime retention also removes the matching archive manifest when it prunes an
  old archive CSV chunk. The global event JSONL and plant-model captures are
  intentionally not pruned by this first-pass cleanup.
- `analyze report` summarizes one ingested run (the most recent run unless
  `--run <id>` or `--session <ts>` is given). It reports idle/load/cooldown
  `p50`/`p90`/`max` for `cpu_tctl_c` and `gpu_memjn_c`/`gpu_envelope_c`,
  per-channel `setpoint_pct`/`duty_pct`/`rpm` plus max boosts, write count and
  up/down setpoint reversals, the response delay from the first
  `--load-threshold-c` crossing (default `75` C) to the first controlled-channel
  setpoint increase above its idle baseline, and authority/write/restore failure
  counts. The idle band is the ticks whose elapsed time is below
  `--idle-seconds` (default `300`, matching the evaluation passes); percentiles
  use nearest-rank on sorted ascending values where `p100` is the maximum.
  `--json` emits the same metrics as a JSON object. `analyze report` is
  read-only; it never writes to fans, the runtime, or the database.

Post-run response-test summaries and decision records:

```powershell
python scripts\analyze_control_run.py `
  --csv release\runtime\logs\archive\svg_mb_control_control-loop_<timestamp>.csv `
  --events release\runtime\logs\svg_mb_control_events.jsonl `
  --status release\runtime\control_runtime.json `
  --current-state release\runtime\current_state.json `
  --config release\control.json `
  --profile combined-load `
  --gpu-load-threshold-c 70 `
  --notes "ambient and subjective noise notes" `
  --out runtime\analysis\combined-load-summary.md `
  --manifest-out runtime\analysis\combined-load-manifest.json
```

When `--out` writes a Markdown summary, the analyzer also writes a sibling
`*.decision.md` record automatically. The generated decision record includes
run identity, artifact hashes, response metrics, channel attribution counts,
and automatic flags for hot-but-low/no-response runs. Use
`--decision-record-out <path>` or `--decision-record-out auto` to override the
path, and `--no-decision-record` to suppress it.

Local eval dashboard:

```powershell
.\scripts\Start-EvalDashboard.ps1 -Open
```

This serves the repo root and opens
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
  Lanes `2,3` are included for the higher-floor front-intake response and use
  one-step rate-limited fan commands.
- The packaged control loop uses GPU envelope curves plus per-channel
  `cpu_override_curve` overlays, commanding the higher duty so Cinebench plus
  max CUDA load has a CPU response path without raising idle floors.
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
- `logs\archive\svg_mb_control_<mode>_<timestamp>.csv`
- `logs\svg_mb_control_evidence.csv`
- `logs\svg_mb_control_evidence_events.jsonl`
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

## Documentation

- `docs\MEASUREMENT_GATE.md`
- `docs\CONTROL_LOOP.md`
- `docs\CONTROL_PIPELINE_MATH.md`
- `docs\READ_LOOP.md`
- `docs\WRITE_ORCHESTRATION.md`
- `docs\RUNTIME_HOME.md`
- `docs\RUNTIME_LOGGING_AND_EVALUATION.md`
- `docs\LOGGING_IMPROVEMENT_PLAN.md`
- `docs\STRUCTURE_AND_STABILITY.md`
- `docs\NORMAL_RUNTIME_AIRFLOW_PROFILE.md`
- `docs\response-evaluation-tuning-plan.md`

Use `docs\MEASUREMENT_GATE.md`, `docs\response-evaluation-tuning-plan.md`, and
`docs\RUNTIME_LOGGING_AND_EVALUATION.md` as the controller tuning workflow.
Use `docs\CONTROL_PIPELINE_MATH.md` as the maintained numerical reference for
curve lookup, smoothing, boost composition, low-band behavior, cadence scoring,
and CSV/status identities. The current shipped configs assert a `250 ms`
control/write profile; lowering cadence, enabling an adaptive floor below that
profile, adding live channels, or broader controller strategy changes still
require fresh measurement evidence before changing defaults.
