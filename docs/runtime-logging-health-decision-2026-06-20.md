# Runtime Logging Health Decision - 2026-06-20

**Project:** svg-mb-control
**Status:** Current for FEAT-0022
**Decision id:** D-LOGHEALTH-1
**Companion spec:** `docs/features/FEAT-0022-runtime-logging-failure-visibility.md`

## Context

The 2026-06-20 startup investigation separated current runtime health from
startup/retry noise. The current worker was healthy, but the evidence review
confirmed a logging-health gap: control-loop CSV writes can fail through
`RuntimeCsvLogger::WriteRow(...)`, and `tick_runner.cpp` currently ignores the
returned `bool`.

This is not isolated to one call site. The older EH-2/EH-3/EH-4/EH-5 discovery
finding grouped several related evidence-integrity issues:

- CSV row append/flush failures are not consistently surfaced.
- Status/snapshot publish failure can suppress near-term retry when timestamps
  advance despite a failed write.
- Event-log append failure is frequently ignored even though events are often
  the only structured report of runtime faults.
- Some write-orchestrator reporting still falls back to `stderr` only.

The 2026-06-20 live read also showed that the fixed live mirror can look stale or
empty while the active archive and manifest catch up. That behavior is compatible
with a running mutable active file, but operators and analyzers need a way to
classify it without confusing a read race with a closed-run evidence defect.

## Decision

Promote FEAT-0022 as the current evidence-integrity feature for runtime logging
failure visibility. Treat it as a coherent logging-health surface, not as a
single `WriteRow()` patch.

Slices A/B plus status/snapshot retry correctness and Slice C analyzer/operator
diagnostics are current and implemented.
CSV/archive/mirror/manifest failures are surfaced through logger failure detail
and rate-limited `runtime_logging.csv_write_failed` /
`runtime_logging.csv_write_recovered` events. The event `mode` identifies
`control-loop`, `read-loop`, or `evidence-log`. Event-log append failure is
surfaced through `logging_health.json` at the runtime-home root and through
additive health/status JSON fields; this does not depend on successfully
appending to the failed event log. Status and snapshot publish failures are
surfaced through sticky `runtime_logging.status_publish_*` and
`runtime_logging.snapshot_publish_*` events, and failed control-loop status or
snapshot publication no longer suppresses the next near-term retry. Analyzer
reports compare manifest-declared, archive-ingested, and latest-mirror row
counts and classify mismatches as running warnings or closed-run
suspect-evidence diagnostics.

Default attack order:

1. **Failure plumbing and tests — implemented 2026-06-20:** make callers observe `WriteRow()` failure,
   expose archive/mirror/flush/manifest failure detail from `RuntimeCsvLogger`,
   and add failing-sink tests before changing operator surfaces.
2. **Sticky/rate-limited reporting — implemented 2026-06-20:** emit structured
   CSV write failure/recovery events when the event log is writable. When the
   event log itself is unwritable, write a sticky `logging_health.json` fallback
   at the runtime-home root on first active failure and rewrite it as recovered
   on the next successful append.
3. **Status/snapshot retry correctness — implemented 2026-06-20:** ensure failed
   status/snapshot publish attempts do not advance the retry timer as if
   publication succeeded, and make failure/recovery visible through sticky
   logging-health events.
4. **Analyzer/operator consistency checks — implemented 2026-06-20:** warn on live
   mirror/archive/manifest inconsistencies using running-session tolerance, and
   mark closed inconsistent runs as suspect evidence.
5. **Docs and runtime evidence — current for FEAT-0022:** update `RUNTIME_HOME.md`,
   `RUNTIME_LOGGING_AND_EVALUATION.md`, and the owning spec verification log,
   then validate implementation through `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`.

## Efficient Slices

### Slice A - no schema expansion beyond events/status

- Observe `WriteRow()` returns in `control-loop`, `read-loop`, and
  `evidence-log`.
- Add rate-limited structured events for CSV and manifest sink failures.
- Add tests that inject or simulate failing archive/mirror/manifest writes.

Implemented 2026-06-20. This catches the immediate bug class and keeps the patch
narrow.

### Slice B - last-resort event-log-unwritable signal

Implemented 2026-06-20.

- The fallback is `logging_health.json` at the runtime-home root, not under
  `logs\`, so the marker can still be written when the `logs\` directory or
  event-log file path is the blocked surface.
- The sidecar schema is `svg_mb_control.logging_health.v1`; it records active
  versus recovered state, event-log path, first/last failure time, recovery
  time, failure count, sink/detail, and the failed event type.
- `--health --json` / `--status --json` report additive
  `logging_health_*` and `event_log_failure_*` fields. An otherwise healthy
  running process is degraded while `event_log_failure_active=true`.
- Tests cover an event-log path blocked by a directory, sticky first-failure
  behavior, recovery rewrite, and health degradation from the sidecar.

### Status/snapshot retry correctness

Implemented 2026-06-20.

- Status publication failures emit one sticky
  `runtime_logging.status_publish_failed` event per active failure and a
  `runtime_logging.status_publish_recovered` event after the next successful
  write.
- Control-loop `current_state.json` publication advances
  `last_snapshot_write_time` only after a successful write. A failed publish
  therefore retries on the next tick instead of waiting for the normal snapshot
  interval.
- Control-loop `control_runtime.json` publication keeps `force_status_write`
  active until the status write succeeds, so a failed status publish is retried
  on the next tick instead of falling back to the normal status cadence.
- Read-loop runtime-home and configured snapshot-mirror publication failures
  emit sticky `runtime_logging.snapshot_publish_failed` /
  `runtime_logging.snapshot_publish_recovered` events. The existing
  `last_success_time` path still advances only after telemetry is available and
  outputs publish successfully.
- Tests cover sticky status failure/recovery, prompt control snapshot retry, and
  read-loop snapshot mirror failure/recovery.

### Slice C - analyzer/operator warnings

Implemented 2026-06-20.

- Add a run-level analyzer warning for manifest row-count, active archive, and
  fixed mirror mismatch.
- In running sessions, classify active-file races as warning/tentative.
- In closed archives, classify mismatch as suspect evidence.
- `analyze report` reads `artifacts.csv_latest.path` from the runtime manifest
  when available, counts data rows in the fixed latest mirror, and compares that
  with the manifest-declared `row_count` and the ingested archive row count.
- The diagnostic flags are `running_csv_manifest_consistency_warning` for
  `status="running"` and
  `closed_csv_manifest_consistency_suspect_evidence` for closed runs.
- The JSON report and generated analysis manifest include
  `csv_latest_row_count`; text reports and decision records include the same
  diagnostic flags.

This is off the control hot path and shipped independently after the failure
state existed.

## Related Items To Look At

- EH-3: status/snapshot publish retry suppression when failure still advances
  the publish timestamp. **Closed 2026-06-20** for control status,
  control-loop `current_state.json`, and read-loop runtime-home/mirror snapshot
  publication.
- EH-4: event-log append failure and the need for a last-resort signal.
- EH-5: `stderr`-only write-orchestrator reporting paths.
- `RuntimeCsvLogger::is_open()` semantics after a stream or flush failure.
- Manifest `row_count` wording/state when rows are accepted into memory but not
  proven flushed to both archive and fixed mirror.
- Active CSV chunk/mirror closed-ready semantics. The current rule is "mutable
  while running"; analyzer checks respect that by warning for running sessions
  and treating closed mismatches as suspect evidence.
- W7 from the old discovery note: CSV archive byte-cap/rotation behavior. Keep
  this separate from FEAT-0022 unless evidence shows unbounded active chunks are
  causing the same evidence-loss failure mode.

## Open Questions For Optional Follow-Up

1. Should manifest fields distinguish logical row count from flushed/persisted
   row count, or is a sticky logging-health flag enough?
2. Should read-loop and foreground `evidence-log` use the same logging-health
   event names as control-loop, or should the event payload carry `mode` only?
3. Is CSV archive byte-cap work part of this feature or a later retention spec?

## Current Disposition

Current for FEAT-0022. All `REQ-LOGHEALTH-*` requirements are implemented and
verified by `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` on 2026-06-20. Optional
manifest persisted-count semantics and CSV archive byte-cap work remain outside
the required FEAT-0022 scope unless later evidence promotes them.
