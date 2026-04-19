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
- `logs\archive\svg_mb_control_<mode>_<timestamp>.csv`

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

`read-loop` writes a poll/status view:

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
- `event_log_path`

`restart_count` and `child_pid` remain `0` in the direct-only runtime.

`control-loop` writes a control-status view:

- `schema_version`
- `mode`
- `status`
- `status_detail`
- `loop_tick_count`
- `loop_last_evaluation`
- `log_csv_path`
- `event_log_path`
- `controlled_channels`

Each controlled-channel entry includes:

- `channel`
- `total_writes`
- `last_setpoint_pct`
- `last_observed_temp_c`
- `baseline_captured`

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
- `svg_mb_control_events.jsonl` stores append-only JSONL events for starts,
  rotations, write attempts, restores, reconcile work, and failures. Its path
  is surfaced in `control_runtime.json` as `event_log_path`.

Archive chunk rotation and pruning are controlled by `log_rotate_hours` and
`log_retain_days` in the control config.

## Ownership Rules

- Control is the only writer of these files.
- `snapshot_path`, when configured, is a mirror target for `current_state.json`;
  it is not a separate authority.
- Startup reconciliation uses `pending_writes.json` to restore incomplete writes
  before any requested mode begins.
- Logging is product-owned inside this repo; it must not delegate CSV or event
  emission to external helper repos or sibling-runtime processes.
