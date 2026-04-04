# Runtime Home

The runtime home is a Control-owned directory used by `--mode read-loop`.

## Location

Resolution precedence, highest first:

1. `runtime_home_path` field from the loaded control config
2. `runtime/` next to `svg-mb-control.exe`
3. `runtime/` under the current working directory

The directory is created at read-loop start via
`std::filesystem::create_directories` if it does not exist.

`runtime_home_path` in the config is resolved relative to the config file's
parent directory when given as a relative path.

## `control_runtime.json`

One status file, rewritten on every poll cycle.

### Publish mechanism

Written to `control_runtime.json.tmp` and published via
`std::filesystem::rename`. This is atomic on Windows (MoveFileExW with
MOVEFILE_REPLACE_EXISTING), so external readers do not observe torn content.

### Schema

```json
{
  "schema_version": 1,
  "status": "running",
  "status_detail": "spawning child",
  "last_refresh": "2026-04-05T12:34:56",
  "snapshot_source": "...absolute path...",
  "restart_count": 0,
  "skipped_polls": 0,
  "successful_polls": 42,
  "stale": false,
  "child_pid": 0
}
```

### Field definitions

- `schema_version`: integer. Always `1` in Phase 1.
- `status`: one of `running`, `shutdown`, `child-died`.
- `status_detail`: human-readable detail string for the last state transition.
- `last_refresh`: local time ISO 8601 of the most recent successful
  `snapshot_path` parse. Empty string until the first successful parse.
- `snapshot_source`: absolute path of the Bench snapshot file being polled.
- `restart_count`: number of times the Bench child has been re-spawned.
- `skipped_polls`: count of polls that observed an unparseable snapshot (for
  example, a torn Bench write) after exhausting the read retry budget.
- `successful_polls`: count of polls that produced a parsed snapshot.
- `stale`: `true` when time since the last successful parse exceeds
  `staleness_threshold_ms`.
- `child_pid`: currently unused. Reserved for Phase 2.

## Log files

Phase 1 does not write separate log files to the runtime home. Bench child
stdout and stderr are drained into bounded in-memory tail buffers on the
supervisor and are not persisted in Phase 1.

## Cleanup

The runtime home is not cleaned up on Control exit. Operators may delete it
between runs.
