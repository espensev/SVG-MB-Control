# Read Loop

## Purpose

`read-loop` is the long-running direct telemetry publisher. It samples AMD, GPU,
and fan state in-process, then republishes Control-owned JSON into the runtime
home.

## Inputs

Top-level config fields used by `read-loop`:

- `runtime_home_path`
- `poll_ms`
- `staleness_threshold_ms`
- `log_rotate_hours`
- `log_retain_days`
- `csv_flush_interval_rows`
- `snapshot_path`
- `runtime_policy_path`

`runtime_policy_path` does not change the loop shape, but it does affect the
fan-policy metadata published in `current_state.json`.

## Outputs

`read-loop` always writes:

- `runtime\current_state.json`
- `runtime\control_runtime.json`
- `runtime\logs\svg_mb_control_output.csv`
- `runtime\logs\archive\svg_mb_control_read-loop_<timestamp>.csv`
- `runtime\logs\svg_mb_control_events.jsonl`

If `snapshot_path` is configured, it also mirrors the same current-state JSON to
that location.

## Runtime Flow

1. Resolve config and runtime home.
2. Resolve runtime policy, if configured.
3. Initialize the direct fan backend.
4. On each poll, sample AMD, GPU, and fan telemetry in-process.
5. Append the sampled row to the active CSV chunk and mirror it to the fixed
   live CSV path.
6. Publish `current_state.json` into the runtime home.
7. Update `control_runtime.json` with poll counters, freshness, status, and the
   active log paths.
8. Append durable JSONL events for loop start, rotations, sample failures, and
   shutdown.
9. Sleep until the next poll, `RequestStop()`, or `stop.request.json`.

## Status File

`control_runtime.json` for `read-loop` carries:

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

`restart_count` and `child_pid` are retained for schema stability in the direct
runtime and remain `0`. `process_id` is the active read-loop worker PID used by
`svg-mb-control --status`.

## Failure Behavior

- If direct fan-writer initialization fails, the loop exits with
  `status="direct-read-failed"`.
- If a single sample fails, the loop records a skipped poll and continues.
- `stale` flips to `true` once the time since the last successful refresh
  exceeds `staleness_threshold_ms`, or `poll_ms * 3` when no explicit threshold
  is configured.
- Log chunk rotation, retention, and CSV flush cadence are controlled by
  `log_rotate_hours`, `log_retain_days`, and `csv_flush_interval_rows`.
- Poll-rate changes should wait for the cadence and timing characterization gate
  in `docs\MEASUREMENT_GATE.md`.

## Shutdown

`Ctrl+C` and `Ctrl+Break` call `RequestStop()`. `svg-mb-control --stop` writes
`stop.request.json` in the runtime home, which the loop checks during polling
and wait periods. The loop finishes the current wait cycle, writes
`status="shutdown"` / `status_detail="stop requested"`, and exits cleanly.
