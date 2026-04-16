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
- `snapshot_path`
- `runtime_policy_path`

`runtime_policy_path` does not change the loop shape, but it does affect the
fan-policy metadata published in `current_state.json`.

## Outputs

`read-loop` always writes:

- `runtime\current_state.json`
- `runtime\control_runtime.json`

If `snapshot_path` is configured, it also mirrors the same current-state JSON to
that location.

## Runtime Flow

1. Resolve config and runtime home.
2. Resolve runtime policy, if configured.
3. Initialize the direct fan backend.
4. On each poll, sample AMD, GPU, and fan telemetry in-process.
5. Publish `current_state.json` into the runtime home.
6. Update `control_runtime.json` with poll counters, freshness, and status.
7. Sleep until the next poll or stop request.

## Status File

`control_runtime.json` for `read-loop` carries:

- `status`
- `status_detail`
- `last_refresh`
- `snapshot_source`
- `successful_polls`
- `skipped_polls`
- `stale`
- `restart_count`
- `child_pid`

`restart_count` and `child_pid` are retained for schema stability in the direct
runtime and remain `0`.

## Failure Behavior

- If direct fan-writer initialization fails, the loop exits with
  `status="direct-read-failed"`.
- If a single sample fails, the loop records a skipped poll and continues.
- `stale` flips to `true` once the time since the last successful refresh
  exceeds `staleness_threshold_ms`, or `poll_ms * 3` when no explicit threshold
  is configured.

## Shutdown

`Ctrl+C`, `Ctrl+Break`, and normal process stop requests call `RequestStop()`.
The loop finishes the current wait cycle, writes
`status="shutdown"` / `status_detail="stop requested"`, and exits cleanly.
