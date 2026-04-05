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

## `pending_writes.json`

Phase 2 adds a second file owned by Control in the runtime home. It
records fan writes that have been requested but not yet confirmed
restored. Every Control startup reconciles this file before doing
anything else.

### Publish mechanism

Written to `pending_writes.json.tmp` and published via
`std::filesystem::rename`. Atomic on Windows.

### Schema

```json
{
  "schema_version": 1,
  "entries": [
    {
      "channel": 3,
      "baseline_duty_raw": 128,
      "baseline_mode_raw": 5,
      "target_pct": 60.0,
      "requested_hold_ms": 30000,
      "bench_started_iso": "2026-04-05T13:10:22",
      "bench_child_pid": 0
    }
  ]
}
```

### Field definitions

- `schema_version`: integer. Always `1` in Phase 2.
- `entries`: array. At most one entry per channel (upsert replaces).
- `channel`: integer 0-6. Target fan channel.
- `baseline_duty_raw`: integer 0-255. Pre-write duty register value
  captured from a `read-snapshot` snapshot.
- `baseline_mode_raw`: integer 0-255. Pre-write mode register value.
- `target_pct`: number. The duty target Control requested.
- `requested_hold_ms`: integer. Control's requested hold duration.
- `bench_started_iso`: string. Local time when Control wrote the entry.
- `bench_child_pid`: integer. Reserved for future use (Phase 2 writes 0).

### Entry lifecycle

1. Control writes the entry before spawning `set-fixed-duty`.
2. Control removes the entry after `set-fixed-duty` exits with code 0.
3. If the child exits non-zero or Control is killed, the entry stays
   in the file.
4. On next Control startup, reconciliation invokes
   `restore-auto --channel <N> --saved-duty-raw <d> --saved-mode-raw <m>`
   for each entry. Successful restore removes the entry. Failed restore
   keeps the entry and blocks Control startup.

## Log files

Phase 1 does not write separate log files to the runtime home. Bench child
stdout and stderr are drained into bounded in-memory tail buffers on the
supervisor and are not persisted in Phase 1.

## Cleanup

The runtime home is not cleaned up on Control exit. Operators may delete it
between runs.
