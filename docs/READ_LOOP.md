# Read Loop

The read loop is Phase 1's persistent read-only supervisor. It is selected
with `--mode read-loop`.

## Lifecycle

1. Resolve config, Bench exe path, and runtime home.
2. Create the runtime home directory.
3. Spawn one `svg-mb-bench.exe logger-service --duration-ms <N>` child, where
   `<N>` is `logger_service_duration_ms` from the config.
4. Enter the poll loop.
5. On child exit, restart up to `child_restart_budget` times with
   `child_restart_backoff_ms` between attempts.
6. On Ctrl+C, Ctrl+Break, or console close, stop cooperatively: send
   CTRL_BREAK_EVENT to the child, wait up to 2 seconds, then exit.

## Poll cycle

Each cycle:

1. Check if the supervised child is still running. If not, apply the restart
   policy.
2. Read `snapshot_path`'s last-write timestamp.
3. If the timestamp has changed since the last observed value, attempt to
   read and parse the snapshot. Retry up to `snapshot_read_retry_count`
   times with `snapshot_read_retry_backoff_ms` between attempts. A parsed
   snapshot requires a trailing `}` character to guard against Bench's
   non-atomic `copy_file_replace` publish.
4. If the retry budget is exhausted without a clean parse, increment
   `skipped_polls`.
5. On a clean parse, increment `successful_polls`, update `last_refresh`,
   clear the `stale` flag, and advance the observed timestamp.
6. Compute `stale` by comparing time since the last successful parse
   against `staleness_threshold_ms`.
7. Write `control_runtime.json` (see `RUNTIME_HOME.md`).
8. Sleep until `poll_ms` elapses or Ctrl+Break arrives.

## Restart policy

When `BenchChildSupervisor::IsRunning()` reports false:

- If `restart_count < child_restart_budget`, increment `restart_count`,
  wait `child_restart_backoff_ms`, spawn a new child, and continue the
  loop.
- Otherwise, write the current status with `status: "child-died"` and
  return a non-zero exit code from the read loop.

## Child process isolation

The child is spawned with `CREATE_NEW_PROCESS_GROUP`. This makes the child
insensitive to the console Ctrl+C event that reaches Control, and lets
Control's supervisor deliver CTRL_BREAK_EVENT to the child's process group
on its own schedule. stdout and stderr are captured on separate background
threads to prevent the child from blocking on a full pipe buffer.

## Bounded tail buffers

Captured child stdout and stderr are stored in 64 KiB rolling buffers per
stream on the supervisor. When a buffer exceeds the cap, older bytes are
dropped. These buffers are not persisted in Phase 1.
