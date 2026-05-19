# Runtime Home

## Location

Runtime-home resolution precedence:

1. `runtime_home_path` from the loaded control config
2. `runtime\` next to `svg-mb-control.exe`
3. `runtime\` under the current working directory

## Files

Control owns these files:

- `current_state.json`
- `control_runtime.json`
- `control_supervisor.json`
- `control_health.json`
- `pending_writes.json`
- `stop.request.json`
- `logs\svg_mb_control_output.csv`
- `logs\svg_mb_control_events.jsonl`
- `logs\svg_mb_control_manifest.json`
- `logs\archive\svg_mb_control_<mode>_<timestamp>.csv`
- `logs\archive\svg_mb_control_<mode>_<timestamp>.manifest.json`
- `svg-mb-control.supervisor.stdout.log`
- `svg-mb-control.supervisor.stderr.log`
- `svg-mb-control.worker.stdout.log`
- `svg-mb-control.worker.stderr.log`

The JSON files remain the authoritative live state and recovery plane. The log
files add history and operator traceability; they do not replace the JSON
contract.

## current_state.json

Published by `one-shot`, `read-loop`, and `control-loop` payload builders.

Key fields:

- `snapshot_time`
- `policy_writes_enabled_present`
- `policy_writes_enabled`
- `amd_sensors`
- `gpu`
- `fans`

Each fan entry can include:

- `channel`
- `label`
- `rpm`
- `tach_raw`
- `duty_raw`
- `mode_raw`
- `duty_percent`
- `tach_valid`
- `manual_override`
- `write_allowed`
- `policy_blocked`
- `effective_write_allowed`

## control_runtime.json

`read-loop` writes a poll/status view with schema version `1`:

- `status`
- `mode`
- `process_id`
- `status_detail`
- `last_refresh`
- `snapshot_source`
- `successful_polls`
- `skipped_polls`
- `stale`
- `restart_count`
- `child_pid`
- `log_csv_path`
- `log_manifest_path`
- `event_log_path`

`process_id` is the active worker PID. `svg-mb-control --status` uses it to
distinguish an active loop from a stale status file. `restart_count` and
`child_pid` remain `0` in the direct-only runtime.

`control-loop` writes a control-status view with schema version `4`:

- `schema_version`
- `mode`
- `process_id`
- `status`
- `status_detail`
- `loop_tick_count`
- `loop_last_evaluation`
- `loop_started_wall_clock`
- `loop_finished_wall_clock`
- `loop_work_duration_ms`
- `loop_intended_interval_ms`
- `loop_achieved_interval_ms`
- `loop_slip_ms`
- `loop_overrun`
- `process_cpu_delta_ms`
- `process_cpu_pct`
- `process_working_set_bytes`
- `process_private_bytes`
- `log_csv_path`
- `log_manifest_path`
- `event_log_path`
- `last_successful_restore_time`
- `controlled_channels`

`last_successful_restore_time` is the local ISO 8601 time of the most recent
successful baseline restore in the current worker process. It is an empty string
until the worker completes a restore. The field is added to the existing schema
version `4`; consumers must tolerate its absence in older status files.

Timing fields describe the most recently completed control-loop tick. The first
tick has no previous tick-start sample, so `loop_achieved_interval_ms` and
`loop_slip_ms` may be reported as `0` in `control_runtime.json`; CSV rows keep
blank numeric cells when a value is unavailable. `loop_achieved_interval_ms` is
start-to-start timing, while `loop_work_duration_ms` is the work done before the
fixed-start scheduler wait.

Each controlled-channel entry includes:

- `channel`
- `total_writes`
- `last_setpoint_pct`
- `last_raw_demand_pct`
- `last_smoothed_demand_pct`
- `last_thermal_pressure_boost_pct`
- `last_cpu_low_soak_boost_pct`
- `last_response_source`
- `last_write_reason`
- `last_observed_temp_c`
- `sensor_failed`
- `consecutive_sensor_failures`
- `circuit_breaker_open`
- `consecutive_write_failures`
- `baseline_captured`

`control_runtime.json` is a status publication. In the current implementation,
it is rate-limited and should not be treated as a per-tick log. Use the active
CSV chunk for per-tick analysis.

## control_supervisor.json

Written by the in-process supervisor (`--run-supervisor`, used by supervised
`read-loop` and `control-loop` launches) with schema version `1`. The worker
rewrites `control_runtime.json` atomically every tick, so supervisor-owned
state is published in this separate sidecar instead of being merged into the
worker status file.

Fields:

- `schema_version`
- `supervisor_pid`
- `worker_restart_count`
- `last_worker_pid`
- `last_worker_started_time`
- `last_worker_restart_time`
- `last_worker_exit_time`
- `last_worker_exit_code`

`last_worker_restart_time` and `last_worker_exit_time` are empty strings until
the first worker restart and first worker exit. `last_worker_exit_code` is
`null` until the first worker exit. The supervisor rewrites this file at
supervisor start, each worker start, each worker exit, and each scheduled
restart.

## control_health.json

Written by the `svg-mb-control --health` CLI path with schema version `1`. It
records the most recent health assessment so the watchdog, `--status`, and the
eval dashboard can show the last result without re-evaluating. Pure health
evaluation does not write this file.

Fields:

- `schema_version`
- `last_health_state`
- `last_health_reason`
- `last_health_exit_code`
- `last_health_time`

## Health command

`svg-mb-control --health --json` and `svg-mb-control --status --json` read
`control_runtime.json`, `control_supervisor.json`, `stop.request.json`, and
`pending_writes.json` and emit a schema-versioned health payload. The health
payload merges the supervisor sidecar fields (`supervisor_state_present`,
`supervisor_pid`, `supervisor_active`, `worker_restart_count`,
`last_worker_pid`, `last_worker_started_time`, `last_worker_restart_time`,
`last_worker_exit_time`, `last_worker_exit_code`) and the worker's
`last_successful_restore_time`. `supervisor_state_present` is `false` and the
merged supervisor fields keep their defaults when `control_supervisor.json` is
absent. The `--health` path also persists the assessment to
`control_health.json`. Health states are:

- `healthy`: process is active, status is fresh, and no stop request or degraded
  channel state is present.
- `degraded`: process is active but an operator-visible issue exists, such as an
  open channel breaker or stop request.
- `stale`: process is active but status freshness is older than the configured
  staleness threshold, or telemetry is explicitly marked stale.
- `stopped`: no usable active worker is present.
- `failed`: status JSON or pending-write recovery state is unreadable, or the
  runtime has reported a failed terminal status.

Exit codes are `0` for `healthy`, `1` for `degraded`, `2` for restartable
`stale`/`stopped`, and `3` for `failed`.

## pending_writes.json

Created by `write-once` and `control-loop` before a direct duty write is
applied.

Each entry includes:

- `channel`
- `baseline_duty_raw`
- `baseline_mode_raw`
- `target_pct`
- `requested_hold_ms`
- `started_iso`
- `child_pid`

`child_pid` is retained for schema continuity and is written as `0` by the
current direct runtime.

## stop.request.json

Created by `svg-mb-control --stop` and consumed by `read-loop` and
`control-loop`. A supervised or foreground loop checks this file and exits
through its normal shutdown path. The file is cleared when a new long-running
loop starts, so stale stop requests do not block the next launch.

`svg-mb-control --restart` writes the same request, waits for the active worker
to publish `status="shutdown"` or stop owning its `process_id`, and only then
launches a new supervisor.

Fields:

- `schema_version`
- `requested_at`
- `reason`

## logs\

`read-loop` and `control-loop` can publish historical CSV telemetry and a
shared event log under `runtime\logs\`.

- `svg_mb_control_output.csv` is the fixed-path live mirror of the active CSV
  chunk.
- `archive\svg_mb_control_<mode>_<timestamp>.csv` stores rotated mode-specific
  CSV chunks. The active chunk path is surfaced in `control_runtime.json` as
  `log_csv_path`.
- `svg_mb_control_manifest.json` is the fixed-path manifest for the active
  runtime log bundle. It records row count, event count, producer identity,
  artifact paths, and `external_logging.required=false`.
- `archive\svg_mb_control_<mode>_<timestamp>.manifest.json` stores the manifest
  beside the matching archive CSV chunk. The latest manifest path is surfaced in
  `control_runtime.json` as `log_manifest_path`.
- `svg_mb_control_events.jsonl` stores append-only JSONL events for starts,
  rotations, write attempts, restores, reconcile work, and failures. Its path
  is surfaced in `control_runtime.json` as `event_log_path`.

Archive chunk rotation and pruning are controlled by `log_rotate_hours` and
`log_retain_days` in the control config. `csv_flush_interval_rows` controls
how often the active CSV archive is flushed and how often pending rows are
written to the fixed-path mirror; `1` preserves per-row mirror refreshes,
while higher values batch mirror writes and disk flushes. Pending mirror rows
are flushed again on rotation and shutdown. The runtime manifest records the
active CSV flush policy and interval. Runtime pruning removes old archive CSV
chunks together with their matching archive manifest sidecar.

For offline cleanup, use `svg-mb-control analyze prune`. It defaults to
dry-run, requires `--apply` for deletion, and only deletes old archive bundles
that have already been ingested into the SQLite analysis database.

For offline evaluation, use `svg-mb-control analyze report` to summarize one
ingested run: idle/load/cooldown `p50`/`p90`/`max` for CPU Tctl and GPU
memory/envelope, per-channel setpoint/duty/RPM and write reversals, response
delay after the first load-threshold crossing, and authority/write/restore
failure counts. It is read-only and does not touch the runtime home.

## Process logs

Supervised launches write process stdout/stderr logs in the runtime root:

- `svg-mb-control.supervisor.stdout.log`
- `svg-mb-control.supervisor.stderr.log`
- `svg-mb-control.worker.stdout.log`
- `svg-mb-control.worker.stderr.log`

The supervisor logs its own startup failures to the supervisor stderr log and
the worker's attached-mode startup/runtime output to the worker logs. Structured
runtime events remain in `logs\svg_mb_control_events.jsonl`.

Control-loop CSV rows include the common telemetry/fan columns plus:

- loop tick and timing-quality fields
- process CPU and memory fields
- per-channel observed temperature, setpoint, feedforward demand, correction,
  thermal-pressure boost, write count, active-write flag, and baseline flag

The JSONL event stream uses schema `svg_mb_control.event.v1`. It is the source
for discrete operational events such as startup, rotation, write attempts,
restore results, policy refusals, sidecar warnings, sensor failure/recovery, and
circuit-breaker transitions.

## Ownership Rules

- Control is the only writer of these files.
- `snapshot_path`, when configured, is a mirror target for `current_state.json`;
  it is not a separate authority.
- Startup reconciliation uses `pending_writes.json` to restore incomplete writes
  before any requested mode begins.
- Logging is product-owned inside this repo; it must not delegate CSV or event
  emission to external helper repos or sibling-runtime processes.
