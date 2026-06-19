# Runtime Startup Investigation - 2026-06-20

## Scope

Read-only investigation of the live Control runtime after the 2026-06-19
23:48 startup sequence. No scheduled task, worker, breaker, or fan-control state
was changed.

## Current State

- `control_health.json` reports `healthy`: runtime process active and status
  fresh at `2026-06-20T00:05:46`.
- `control_runtime.json` reports `status=running`, `mode=control-loop`,
  worker PID `33508`, tick `4270`, achieved interval `250.8287 ms`, slip
  `0.8287 ms`, and work duration `1.2839 ms`.
- `control_supervisor.json` records worker PID `33508`, started at
  `2026-06-19T23:48:46`, with `worker_restart_count=0` and no last exit code.
- Scheduled task status was clean: `SVG-MB Control` last ran at
  `2026-06-19 23:48:01` with result `0`; `SVG-MB Control Watchdog` last ran at
  `2026-06-20 00:06:46` with result `0` and no missed runs.

Conclusion: the current worker is healthy. The noisy evidence belongs to the
startup/retry path before the current worker, not to the running control loop.

## Startup Sequence

Confirmed from `release\runtime\logs\svg_mb_control_events.jsonl`:

- `2026-06-19T23:46:46` - supervisor started.
- `2026-06-19T23:46:49` - worker PID `52940` started, followed by
  `reconcile.restore_applied` events for channels `0`, `4`, `3`, `2`, `5`,
  and `1`.
- `2026-06-19T23:48:01` - supervisor started again.
- `2026-06-19T23:48:17` - `supervisor.force_terminate_failed` with
  `SUPERVISOR_FORCE_TERMINATE_FAILED`: no worker PID was resolved from
  `control_supervisor.json`, so escalation could not act.
- `2026-06-19T23:48:20` - supervisor shutdown.
- `2026-06-19T23:48:46` - supervisor started and worker PID `33508` started.
- `2026-06-19T23:48:47` - `control_loop.start`.
- `2026-06-19T23:48:47` - six tick-1 `control_loop.authority_reasserted`
  events, one for each controlled channel.

No post-start `control_loop.authority_reasserted`,
`supervisor.force_terminate_failed`, or `control_loop.sidecar_flush_failed`
events were found in the checked post-start window.

## Controller Versus Data

The active archive for the current session is
`release\runtime\logs\archive\svg_mb_control_control-loop_20260619_234846.csv`.
The manifest reported a running session with `row_count=3900` at
`2026-06-20T00:05:05`, `csv_flush_interval_rows=4`, and
`mirror_mode=buffered_same_interval`.

Startup CSV statistics for `2026-06-19T23:48:*`:

- Rows inspected: `50`.
- Max slip: `362.222 ms` at tick `10`.
- Slip rows over `10 ms`: `2`; over `50 ms`: `1`.
- Max work duration: `611.865 ms` at tick `9`.
- Top startup setpoints around the slip/work spike were expected warm-start
  response values, for example channel `2` around `55.884-56.584%`, channel `3`
  around `54.650-54.726%`, and channel `4` around `50.460-50.715%`.

Post-start sample from `2026-06-19T23:49` through `2026-06-20T00:0*`:

- Rows inspected: `4226`.
- Max slip: `2.523 ms`.
- Slip rows over `10 ms`: `0`.

Conclusion: the large cadence artifact was isolated to startup. The running loop
settled into the expected 250 ms cadence.

## CSV And Manifest Observability

During the live read, `logs\svg_mb_control_output.csv` briefly appeared stale or
empty while the active archive and manifest later caught up. This is consistent
with the documented mutable-active-file behavior in
`docs\RUNTIME_LOGGING_AND_EVALUATION.md`: active CSV files have no closed/ready
marker and readers must treat them as mutable while Control is running.

There is still a real observability target in source:

- `src\control\tick_runner.cpp` calls `csv_logger.WriteRow(...)` in the
  control loop and ignores the returned `bool`.
- `src\runtime\runtime_csv_archive.cpp` returns `false` from `WriteRow(...)` on
  archive or mirror flush/write failure, but that result is not surfaced by the
  control loop.
- `RuntimeCsvLogger::WriteManifest(...)` calls `FlushStreams()` when rows are
  pending but does not propagate that failure into a runtime event or health
  signal.

An older discovery note already records this family of discarded write/append
returns under EH-2/EH-3/EH-4/EH-5, but that file is historical context under the
repo navigation rules. The gap should be promoted through the current
feature/backlog path before expanding runtime event, status, health, or manifest
schema behavior.

## Targets To Investigate

1. Promote a current logging-health follow-up for rate-limited CSV
   write/flush/manifest failure visibility. This is now tracked as
   `docs\features\FEAT-0022-runtime-logging-failure-visibility.md`; the
   implementation gate is D-LOGHEALTH-1 in
   `docs\runtime-logging-health-decision-2026-06-20.md`.
2. Add an analyzer or operator check that warns when the fixed live mirror and
   active archive/manifest are inconsistent during a running session, while
   preserving the documented rule that the active file is mutable.
3. Revisit FEAT-0008 post-v1 pre-first-write PID handling only if the no-PID
   `supervisor.force_terminate_failed` noise recurs during normal watchdog
   recovery. The current event was from startup/retry, and the current worker is
   healthy.
4. Do not tune cadence or fan curves based on this startup spike. The post-start
   evidence did not show recurring slip.
5. Continue using pinned closed archives for analysis when comparing controller
   response. Avoid interpreting a single live mirror read as authoritative.
