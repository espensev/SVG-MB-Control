# FEAT-0022: Runtime logging failure visibility

**Project:** svg-mb-control
**Status:** Implemented   **Version:** 0.5   **Updated:** 2026-06-20
**Namespace:** `REQ-LOGHEALTH-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`,
`docs/STRUCTURE_AND_STABILITY.md`, `docs/CONTROL_LOOP.md`,
`docs/READ_LOOP.md`, `docs/RUNTIME_HOME.md`,
`docs/RUNTIME_LOGGING_AND_EVALUATION.md`,
`docs/runtime-startup-investigation-2026-06-20.md`,
`docs/runtime-logging-health-decision-2026-06-20.md`
**Purpose:** make runtime logging and evidence-sink failures visible without
changing fan-control behavior.

## 1. Summary

The controller already treats fan writes and sidecar failures as observable
runtime events, but several evidence sinks still return failure in source without
a current operator-visible signal. This feature captures a logging-health surface
for CSV row writes, archive/mirror flushes, manifest writes, status/snapshot
publishes, and event-log append failure so operators and analyzers can tell when
runtime evidence may be incomplete.

Version 0.5 implements Slices A/B, status/snapshot retry correctness, and Slice
C analyzer/operator consistency warnings. Slice A added
CSV/archive/mirror/manifest failure detail, caller-side `WriteRow(...)` checks,
and rate-limited CSV write failure/recovery events. Slice B adds the
last-resort `logging_health.json` sidecar and health degradation for active
event-log append failure. The status/snapshot slice makes failed status and
snapshot publishes visible, keeps control status retry active after a failed
publish, and advances the control snapshot retry timestamp only after a
successful `current_state.json` publish. Slice C adds analyzer report
diagnostic flags that classify CSV manifest/archive/latest-mirror row-count
inconsistency as a running mutable warning or a closed-run suspect-evidence
defect.

## 2. Problem & motivation  *(promotion gate 1)*

The 2026-06-20 startup investigation found a current healthy worker but also
confirmed an evidence-integrity gap: `src\control\tick_runner.cpp` calls
`csv_logger.WriteRow(...)` and ignores the returned `bool`; the logger can return
`false` for archive or mirror write/flush failure. The same investigation also
observed that the fixed live mirror can briefly look stale or empty while the
active archive/manifest later catch up, which is documented mutable-active-file
behavior but still easy to misread during operator checks.

The older discovery note
`docs\archive\discovery-loop-plan-tighter-pass-2026-06-14.md` records the broader
EH-2/EH-3/EH-4/EH-5 family: discarded CSV row returns, status/snapshot publish
return handling, event-log append returns that are often ignored, and a
`cerr`-only write-orchestrator reporting path. That file is historical context;
this spec promotes the evidence-integrity target into the current feature
system before implementation.

## 3. Goals & non-goals

**Goals**

- Surface CSV archive, fixed mirror, flush, and manifest write failures from the
  control-loop and read/evidence logging paths.
- Surface status/snapshot publish failure without suppressing near-term retry by
  advancing retry timestamps on failure.
- Define a last-resort signal for event-log append failure, because an event-log
  failure cannot rely on the event log as its only report.
- Add analyzer/operator checks for active archive, fixed mirror, and manifest
  inconsistency that distinguish a running mutable session from a closed corrupt
  or incomplete capture.
- Keep all reporting rate-limited or sticky enough to avoid per-tick log storms.

**Non-goals**

- No fan duty, setpoint, curve, cadence, breaker, restore, or write-policy
  behavior change.
- No guarantee that every row is durably on disk before the next tick.
- No synchronous flush on every row unless a later decision records explicit
  runtime evidence and accepts the cost.
- No use of logging-health state as a control input.
- No sibling repo, subprocess bridge, HWiNFO, or external monitor dependency.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| Repo stays standalone | `AGENTS.md` §Repo Boundary | Visibility is produced by in-repo runtime logging/analyzer code only. |
| Live Runtime Safety | `AGENTS.md` §Live Runtime Safety | The feature is observational. Runtime restart, scheduled-task changes, breaker resets, or fan writes are not part of implementation or validation without explicit live authorization. |
| Measurement Gate baseline | `docs/MEASUREMENT_GATE.md` | Reporting must not change the shipped 250 ms tick, 250 ms cooldown, channel set, or control strategy. If a fallback sink adds measurable work, runtime evidence is required before promotion. |
| Control-computation identity | `docs/CONTROL_PIPELINE_MATH.md` | Logging-health state is not an input to source selection, smoothing, boosts, deadband, safety override, or write gates. |
| Runtime schema stability | `docs/RUNTIME_HOME.md` | Any event/status/health/manifest fields are additive and nullable; existing runtime files and archives remain valid. |

## 5. Behavior specification

Runtime code that writes evidence sinks must not discard failure results silently.
For v1, the affected sinks are:

- control-loop CSV row writes through `RuntimeCsvLogger::WriteRow(...)`;
- read-loop/evidence-log CSV row writes through the same logger;
- archive stream flushes, fixed mirror flushes, and manifest writes in
  `runtime_csv_archive.cpp`;
- runtime status/snapshot publication from the control and read loops; and
- runtime event-log append calls where failure is currently invisible to the
  operator.

The implementation must introduce one coherent logging-health reporting path
instead of separate ad hoc patches. The exact fields/events are settled by
D-LOGHEALTH-1 before implementation, but the behavior must provide:

- a rate-limited event or equivalent operator-visible signal for CSV and
  manifest failures when the event log is writable;
- a sticky status/health or last-resort runtime-home signal for cases where the
  event log itself is not writable;
- enough detail to distinguish archive, mirror, manifest, status/snapshot, and
  event-log failure classes;
- recovery indication when the failing sink later succeeds; and
- analyzer/report logic that marks a run as having incomplete or suspect
  evidence instead of silently treating it as clean.

The active CSV mirror remains mutable while Control is running. Analyzer or
operator checks must not report a failure merely because the mirror is within
the configured CSV flush interval or because a live file is read mid-append. The
check should become strict for closed archives and should warn, not fail, when a
running session's mirror/archive/manifest are temporarily inconsistent.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-LOGHEALTH-01 | Control-loop, read-loop, and evidence-log callers must observe `RuntimeCsvLogger::WriteRow(...)` failure and route it through the logging-health reporting path instead of discarding the return value. |
| REQ-LOGHEALTH-02 | `RuntimeCsvLogger` must expose archive stream, fixed mirror, flush, and manifest write failures with enough sink detail for operator/analyzer diagnosis. |
| REQ-LOGHEALTH-03 | Runtime status/snapshot publication must not advance its retry timestamp after a failed publish, and the failure must be visible through the logging-health path. |
| REQ-LOGHEALTH-04 | Event-log append failure must have a visible last-resort or sticky signal that does not depend on successfully appending to the same failed event log. |
| REQ-LOGHEALTH-05 | Logging-health failure and recovery signals must be rate-limited or sticky so a persistent sink failure cannot emit one full event per tick indefinitely. |
| REQ-LOGHEALTH-06 | Analyzer/operator checks must classify active archive, fixed mirror, and manifest inconsistency as running-mutable warning versus closed-run evidence defect. |
| REQ-LOGHEALTH-07 | Logging-health state must remain observational and must not affect fan duty, setpoint computation, source selection, write gates, breaker state, restore behavior, cadence, or channel policy. |
| REQ-LOGHEALTH-08 | Runtime-home/event/status/manifest changes must be additive and backward-compatible with older files and archives. |

## 7. Data / schema deltas

Implemented deltas:

- Runtime event types `runtime_logging.csv_write_failed` and
  `runtime_logging.csv_write_recovered` for CSV/archive/mirror/manifest
  visibility when the event log is writable.
- A sticky runtime-home sidecar `logging_health.json` for event-log-unwritable
  state. It uses schema `svg_mb_control.logging_health.v1` and records
  `event_log_failure_active`, `event_log_writable`, `event_log_path`,
  first/last failure times, recovery time, failure count, sink/detail, and the
  failed event type.
- `--health --json` / `--status --json` include additive
  `logging_health_*` and `event_log_failure_*` fields and degrade an otherwise
  healthy active runtime while `event_log_failure_active=true`.
- Runtime event types `runtime_logging.status_publish_failed` and
  `runtime_logging.status_publish_recovered` for sticky status publication
  failure/recovery visibility.
- Runtime event types `runtime_logging.snapshot_publish_failed` and
  `runtime_logging.snapshot_publish_recovered` for sticky control/read snapshot
  publication failure/recovery visibility. Control-loop snapshot publish timing
  advances only on successful `current_state.json` writes.
- `analyze report` reads `artifacts.csv_latest.path` from the runtime manifest
  when available, counts data rows in that fixed latest mirror, and includes
  additive `csv_latest_path` / `csv_latest_row_count` fields in JSON report
  output and analysis manifests.
- `analyze report` emits diagnostic flags
  `running_csv_manifest_consistency_warning` and
  `closed_csv_manifest_consistency_suspect_evidence` when manifest-declared,
  archive-ingested, or latest-mirror row counts disagree.

Deferred deltas:

- Optional manifest fields for logging-health state and/or persisted-row
  clarity if a later slice chooses that route.

No existing runtime-home file, archive, CSV row, manifest, or config may become
invalid. If implementation adds status, health, manifest, or event fields, update
`docs/RUNTIME_HOME.md`; if analyzer/report behavior changes, update
`docs/RUNTIME_LOGGING_AND_EVALUATION.md`.

## 8. CLI / config / operator surface deltas

No new live-control CLI is required. Slice A adds an event-only operator surface:

- `runtime_logging.csv_write_failed`
- `runtime_logging.csv_write_recovered`
- `runtime_logging.status_publish_failed`
- `runtime_logging.status_publish_recovered`
- `runtime_logging.snapshot_publish_failed`
- `runtime_logging.snapshot_publish_recovered`

The event `mode` identifies `control-loop`, `read-loop`, or `evidence-log`.
Failure events include the active CSV path, event-log path, and a `detail`
string containing the failing logger sink when available.

Slice B adds an additive last-resort operator surface:

- `logging_health.json` at the runtime-home root records active/recovered
  event-log append failure without depending on the failed event log.
- `--health --json` / `--status --json` report
  `logging_health_present`, `event_log_failure_active`,
  `event_log_failure_count`, `event_log_failure_path`,
  `event_log_failure_sink`, and `event_log_failure_detail`.
- A running otherwise-healthy process is `degraded` while
  `event_log_failure_active=true`.

Slice C adds an analyzer/operator report surface:

- `analyze report --json` includes `run.csv_latest_path`,
  `run.csv_latest_row_count`, and one of the diagnostic flags above when row
  counts disagree.
- Text reports and generated decision records include the same diagnostic flags.
- A running manifest mismatch is a warning because active files are mutable; a
  closed-run mismatch is suspect evidence.

Any later CLI flag, manifest field, or health/status schema change must be
settled in a follow-up decision update and documented before implementation.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/runtime-logging-health-decision-2026-06-20.md`](../runtime-logging-health-decision-2026-06-20.md) (D-LOGHEALTH-1) | Slice A event surface, Slice B event-log fallback path, status/snapshot retry behavior, Slice C analyzer thresholds, and implementation order; manifest persisted-count semantics remains a separate optional follow-up decision. | Current for FEAT-0022 |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-LOGHEALTH-01 | T, R | Unit/smoke tests with a failing `RuntimeCsvLogger` or sink seam; review control/read/evidence callers no longer discard `WriteRow(...)` failure. |
| REQ-LOGHEALTH-02 | T, R | `runtime_csv_archive` tests for archive, mirror, flush, and manifest failure classification; review sink detail in event/status/health/manifest docs. |
| REQ-LOGHEALTH-03 | T, R | Status/snapshot publish failure test confirms retry timestamp advances only on success; review against `RUNTIME_HOME.md`. |
| REQ-LOGHEALTH-04 | T, R | Event-log unwritable test confirms a last-resort or sticky signal remains visible without relying on event append success. |
| REQ-LOGHEALTH-05 | T, R | Persistent-failure test confirms rate limit/sticky behavior and recovery signal; review no per-tick event storm. |
| REQ-LOGHEALTH-06 | T, R, M | Analyzer/operator tests for running mutable versus closed inconsistent archive/mirror/manifest states; optional read-only runtime evidence from a live running session. |
| REQ-LOGHEALTH-07 | T, R | Control-loop regression tests and review prove logging-health state is not read by control computation, write gates, breaker, restore, cadence, or channel policy. |
| REQ-LOGHEALTH-08 | T, R | Backward-compatibility tests for older runtime files/archives; docs review for additive schema changes. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc, decision record, or source.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Failure surface names and payloads | implemented Slice A | Use `runtime_logging.csv_write_failed` and `runtime_logging.csv_write_recovered`; the event `mode` carries the source mode and `detail` carries the logger sink/detail. |
| Last-resort signal when event append fails | implemented Slice B | Write sticky `logging_health.json` at the runtime-home root so it can still work when `logs\` or the event-log file is blocked; health/status JSON degrades active runtimes while `event_log_failure_active=true`. |
| Status/snapshot retry surface | implemented status retry slice | Use `runtime_logging.status_publish_failed` / `runtime_logging.status_publish_recovered` and `runtime_logging.snapshot_publish_failed` / `runtime_logging.snapshot_publish_recovered`; do not advance control snapshot retry timing on failed publish and keep forced control-status retry active until status publication succeeds. |
| Analyzer warning thresholds for running sessions | implemented Slice C | Any manifest/archive/latest-mirror row-count disagreement is a diagnostic warning while `status="running"` and a suspect-evidence diagnostic for closed runs. No DB schema migration is required. |
| Manifest row-count semantics after failed flush | optional follow-up | Preserve existing manifest compatibility. Current analyzer checks compare declared, archive-ingested, and latest-mirror row counts; a future manifest revision may distinguish logical rows from proven persisted rows if needed. |
| Whether CSV archive byte caps join this feature | follow-up decision | Keep byte-cap retention separate unless later evidence shows unbounded active chunks are part of the same evidence-integrity slice. |

## 12. Measurement gate & dependencies

- **Measurement gate:** this feature does not intentionally change cadence,
  write cooldown, channels, or control strategy. Promotion still requires review
  that failure reporting and fallback writes do not add meaningful work to the
  hot path; if the chosen fallback writes synchronously per tick, runtime
  evidence is required before build authorization.
- **Depends on:** existing `RuntimeCsvLogger`, runtime event log, runtime health
  and status publication, analyzer ingest/report code.
- **Build/test impact:** Slices A/B plus status retry and Slice C add C++
  logger/status/event-log fallback tests, control/read-loop snapshot retry
  tests, analyzer report consistency tests, runtime-health tests, and runtime
  docs updates.
  `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` passed on 2026-06-20.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, Measurement Gate, control identity, and runtime schema stability (§4).
- [x] 3. Required design decision record(s) written and marked current (§9; D-LOGHEALTH-1 is Current for FEAT-0022).
- [x] 4. Concrete `REQ-LOGHEALTH-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks and mirrored in `docs/TRACEABILITY.md` (§10).
- [x] 6. Confirmed it does not violate Live Runtime Safety or Repo Boundary, and does not silently move the Measurement Gate baseline; implemented slices add only event reporting, logger failure detail, status/snapshot retry correctness, a fallback sidecar, health visibility, and analyzer diagnostics.
- [x] 7. Doctrine check: claims are grounded; proposed behavior is labeled proposed; no undefined vague terms.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-LOGHEALTH-01 | pass | `tick_runner.cpp`, `read_loop.cpp`, and `evidence_log.cpp` observe `WriteRow(...)` and emit `runtime_logging.csv_write_failed` on first failure; `Test-LocalCI` passed. | 2026-06-20 |
| REQ-LOGHEALTH-02 | pass | `RuntimeCsvLogger` records `last_error_sink/detail`; `runtime_csv_archive_tests` cover mirror-open and manifest-write failure detail; `Test-LocalCI` passed. | 2026-06-20 |
| REQ-LOGHEALTH-03 | pass | `runtime_status_tests` verifies sticky `runtime_logging.status_publish_failed` / `runtime_logging.status_publish_recovered`; `test_control_loop.py` verifies failed `current_state.json` publish does not advance the retry timer; `test_read_loop.py` verifies snapshot mirror failure/recovery events. `Test-LocalCI` passed. | 2026-06-20 |
| REQ-LOGHEALTH-04 | pass | `runtime_event_log_tests` simulates an unwritable event-log path and verifies sticky `logging_health.json` plus recovered state without relying on the failed event log; `test_runtime_health.py` verifies health degrades while active. `Test-LocalCI` passed. | 2026-06-20 |
| REQ-LOGHEALTH-05 | pass | CSV, status, and snapshot failure events use sticky in-memory state; event-log append failure writes `logging_health.json` once per active failure and rewrites recovery on the next successful append. `Test-LocalCI` passed. | 2026-06-20 |
| REQ-LOGHEALTH-06 | pass | `analyze_report_tests` verifies running mismatches emit `running_csv_manifest_consistency_warning` and closed mismatches emit `closed_csv_manifest_consistency_suspect_evidence`; `test_analyze_ingest.py` verifies `analyze report` reads `csv_latest` row counts and surfaces the flags in JSON/text. `Test-LocalCI` passed. | 2026-06-20 |
| REQ-LOGHEALTH-07 | pass | Review: logging-health events/sidecar/health fields are observational and are not read by setpoint computation, write gates, breaker, restore, cadence, or channel policy; `Test-LocalCI` passed. | 2026-06-20 |
| REQ-LOGHEALTH-08 | pass | Event schema remains `svg_mb_control.event.v1`; CSV/status/snapshot logging-health events, `logging_health.json`, and health JSON fields are additive and optional; runtime docs updated; `Test-LocalCI` passed. | 2026-06-20 |

**Spec vs. implementation deltas:** Slices A/B shipped first, followed by the
status/snapshot retry slice, then Slice C analyzer/operator diagnostics. No
`REQ-LOGHEALTH-*` requirement remains deferred.
