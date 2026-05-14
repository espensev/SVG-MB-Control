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
- `pending_writes.json`
- `logs\svg_mb_control_output.csv`
- `logs\svg_mb_control_events.jsonl`
- `logs\svg_mb_control_manifest.json`
- `logs\archive\svg_mb_control_<mode>_<timestamp>.csv`
- `logs\archive\svg_mb_control_<mode>_<timestamp>.manifest.json`

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

`restart_count` and `child_pid` remain `0` in the direct-only runtime.

`control-loop` writes a control-status view with schema version `3`:

- `schema_version`
- `mode`
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
- `controlled_channels`

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
- `last_observed_temp_c`
- `sensor_failed`
- `consecutive_sensor_failures`
- `circuit_breaker_open`
- `consecutive_write_failures`
- `baseline_captured`

`control_runtime.json` is a status publication. In the current implementation,
it is rate-limited and should not be treated as a per-tick log. Use the active
CSV chunk for per-tick analysis.

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
`log_retain_days` in the control config.

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
