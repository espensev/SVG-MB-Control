# FEAT-0015: Event JSONL has a retention bound

**Project:** svg-mb-control
**Status:** Implemented (2026-06-18; Test-LocalCI green)   **Version:** 0.3   **Updated:** 2026-06-18
**Namespace:** `REQ-EVENTRET-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/RUNTIME_HOME.md`, `docs/RUNTIME_LOGGING_AND_EVALUATION.md`,
`docs/discovery-runtime-disk-growth-2026-06-14.md`
**Purpose:** define the accepted retention direction for the runtime event log
(`logs/svg_mb_control_events.jsonl`), which is append-only with no rotation or
retention and therefore grows without an enforced upper bound.

## 1. Summary

The runtime event log is written one NDJSON line per event by
`AppendRuntimeEvent` (`src/runtime/runtime_event_log.cpp:210`), which opens the
file in append mode and writes a single serialized line per call. There is no
size cap, age cap, rotation, or severity filter on this file: the function only
appends. The CSV evidence path is bounded by `log_rotate_hours` and
`log_retain_days` (`RuntimeCsvLogger`, wired in `src/runtime/evidence_log.cpp:78`),
but those keys govern the archive CSV chunks only; they do not apply to the event
JSONL. On the snapshot recorded in
`docs/discovery-runtime-disk-growth-2026-06-14.md`, the file was 212 MB /
586,825 lines spanning 2026-05-21 to 2026-06-14, of which 579,565 lines (98.8%)
were `control_loop.write_applied` (emitted per applied write at
`src/control/channel_write.cpp:384`).

This is a named design gap, not an unset config value: no retention mechanism for
this file exists in the repo. This spec promotes the accepted direction: age
rotation modelled on the CSV path plus severity-aware persistence so routine
`write_applied` is no longer stored per write. The spec authorizes
implementation of that bounded-retention behavior. The implementation is now in
`runtime_event_log.cpp` and its callers; verification is recorded in §14.

## 2. Problem & motivation  *(promotion gate 1)*

Sourced from observed runtime evidence (`docs/discovery-runtime-disk-growth-2026-06-14.md`,
Finding 1; tracking issue
[espensev/SVG-MB-Control#4](https://github.com/espensev/SVG-MB-Control/issues/4))
and a static-verified code gap.

1. **The event log is append-only with no bound.** `AppendRuntimeEvent`
   (`src/runtime/runtime_event_log.cpp:210-336`) resolves the event-log path,
   serializes one JSON line, and writes it in append mode (`FILE_APPEND_DATA` on
   Windows; `std::ios::app` elsewhere). It contains no rotation, truncation, age
   purge, or severity gate. No other code path rotates or prunes the event JSONL —
   the only retention in this area is `analyze prune` for archive CSV bundles,
   which does not touch this file.

2. **The config keys that bound the CSV path do not apply.** `log_rotate_hours`
   and `log_retain_days` drive `RuntimeCsvLogger` for the archive CSV chunks
   (`src/runtime/evidence_log.cpp:78-87`; `RuntimeCsvLogger.MaybeRotate`). The
   event JSONL is documented as "append-only" with no rotation or retention in
   `docs/RUNTIME_HOME.md`. So the bound is absent by design, not by misconfiguration.

3. **The dominant content is routine, not diagnostic.** 98.8% of recorded lines
   were `control_loop.write_applied`, an `info`-severity per-write record
   (`src/control/channel_write.cpp:384`; severity inferred by `InferSeverity`,
   `runtime_event_log.cpp:156`). A retention model can therefore bound the file
   either by rotation/age (like the CSV path) or by not persisting every routine
   write, while still keeping warnings, errors, and lifecycle events.

   The 2026-06-18 live check after FEAT-0020 confirmed the growth is continuing:
   `release/runtime/logs/svg_mb_control_events.jsonl` was 270.3 MB, while
   `release/runtime/logs` was 3.265 GiB total. This feature owns the event JSONL
   bound; FEAT-0016 owns the analyzer DB bound and the immediate DB reclaim.

4. **Durability sub-finding (in scope for investigation, not yet root-caused).**
   The same investigation observed a NUL byte mid-file, indicating at least one
   partial/torn write at some point. The current append path already writes each
   line in a single call and `CountNonEmptyLines` opens with `FILE_SHARE_DELETE`
   anticipating "a concurrent rename or rotation of the event log" / "the future
   rotator" (`runtime_event_log.cpp:28-40`). The torn-write cause is **not
   confirmed**; any rotation added here must preserve single-line append atomicity
   rather than reintroduce the hazard.

## 3. Goals & non-goals

**Goals**
- Give `logs/svg_mb_control_events.jsonl` an enforced upper bound (by age
  rotation, by severity-based persistence, or a combination), so the file stops
  growing without limit across sessions.
- Keep warnings, errors, and lifecycle/transition events retained for the chosen
  retention window: bounding the file must not silently drop diagnostic events.
- Preserve append durability: any rotation must keep the single-line atomic append
  and must not reintroduce interleaved or torn writes between concurrent emitters.

**Non-goals**
- No change to which events are *emitted* by the control loop, read loop, or
  evidence logger, nor to the event schema vocabulary, beyond a retention/severity
  decision about which are *persisted* and for how long.
- No change to the CSV archive retention (`log_rotate_hours` / `log_retain_days`),
  which is within policy (`docs/discovery-runtime-disk-growth-2026-06-14.md`,
  "Not a problem").
- No change to the analyze SQLite DB retention; that is the separate FEAT-0016
  (`REQ-DBRETAIN-*`), cross-referenced in §12.
- No change to computed duty, cadence, channel set, curve, or mixed-input strategy.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this proposal stays inside it |
|---|---|---|
| Runtime sidecar / status / manifest / log schema stays backward-compatible | `docs/RUNTIME_HOME.md` | The event schema `svg_mb_control.event.v1` (`runtime_event_log.cpp:228`) is unchanged; rotation/retention adds at most an additive config key and rotated-file naming. No existing event JSONL becomes invalid. |
| No fan write / start / stop / breaker reset outside an explicit live task | `AGENTS.md` §Live Runtime Safety | Retention is a log-management decision; it adds no live action, no write site, and does not change any control or recovery path. |
| Shipped 250 ms cadence / channel set / input strategy is the measured baseline | `docs/MEASUREMENT_GATE.md` | Logging-side only; cadence, channels, and input strategy are unchanged, so the gate baseline does not move. Event logging remains best-effort and does not change control decisions. |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | No control term is added or altered; only event-log persistence changes. |
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | The change is confined to in-repo runtime code (`src/runtime/runtime_event_log.cpp` and its callers); no external dependency. |

## 5. Behavior specification

Implemented behavior lives at the event append site `AppendRuntimeEvent`
(`src/runtime/runtime_event_log.cpp`) and the retention registration hook
`ConfigureRuntimeEventLogRetention`.

- **Age rotation (mirrors the CSV path).** `control-loop`, `read-loop`, and
  `evidence-log` register their event paths with `log_rotate_hours` and
  `log_retain_days`. When the active event JSONL's registered active-start time
  is older than the configured rotate age, it is renamed into
  `logs/archive/<active-stem>_<timestamp>.jsonl`; frequent appends do not refresh
  the deadline. Archives older than the retention window are pruned. A `0`
  rotate or retain value disables that part of the bound.
- **Severity-aware persistence.** The control loop samples routine `info`
  `control_loop.write_applied` events (tick 1 and every 240th tick by default).
  Events with `warning`, `error`, or `critical` severity and lifecycle/transition
  events are persisted normally. The CSV path remains the full dense telemetry
  record, so the reduction only affects routine event-log duplication.
- **Durability.** Rotation and append are serialized inside the process registry,
  and each persisted event is still serialized once and written as a single
  append call (`FILE_APPEND_DATA` on Windows). The observed mid-file NUL finding
  is bounded by preserving single-call append behavior and testing whole-line
  JSON across a rotation boundary.
- **Backward compatibility.** No event payload field, schema name, dashboard
  parser contract, or analyze schema changes. Paths without configured retention
  keep the historical append behavior.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-EVENTRET-01 | The runtime event log `logs/svg_mb_control_events.jsonl` must gain an enforced upper bound — size/age rotation, severity-based persistence, or a combination — so it does not grow without limit across sessions. The accepted model and its bound are recorded in §11 and the design decision before implementation. |
| REQ-EVENTRET-02 | The retention mechanism must preserve append durability: each persisted event is written as a single atomic append, and rotation must not interleave or split NDJSON lines between concurrent emitters. The cause of the observed mid-file NUL byte must be confirmed or explicitly bounded before implementation. |
| REQ-EVENTRET-03 | Bounding the file must not silently drop diagnostic events: every `warning`/`error`/`critical` event (`InferSeverity`) and lifecycle/transition event must remain persisted within the accepted retention window even if routine `info` `write_applied` events are reduced or rotated out. |
| REQ-EVENTRET-04 | The change must be backward-compatible and additive: the `svg_mb_control.event.v1` event schema is unchanged, any new config key defaults to current behavior when absent, and existing event JSONL files plus the `CachedEventCount` and dashboard consumers stay valid. |
| REQ-EVENTRET-05 | The change must be confined to event-log management: computed duty, cadence, channel set, control-computation identity, and the CSV archive retention are unchanged, event logging remains best-effort and does not change control decisions, and `docs/RUNTIME_HOME.md` / `docs/RUNTIME_LOGGING_AND_EVALUATION.md` are updated at implementation. |

## 7. Data / schema deltas

- New/changed fields: none in the event payload — `svg_mb_control.event.v1`
  (`runtime_event_log.cpp:228`) is unchanged. Rotation introduces archived
  event-file naming alongside the active `svg_mb_control_events.jsonl` /
  `svg_mb_control_evidence_events.jsonl` (`EvidenceLogArtifactNaming`,
  `evidence_log.cpp:57`); naming follows the existing archive convention.
- Config impact (`config/control.*.json`, `config/machines/*.json`): no new key.
  Event retention reuses `log_rotate_hours` / `log_retain_days`; absent retention
  registration or `0` values preserve append-only behavior.
- Schema/version impact: none to the analyze schema or the event schema; this is
  log file management. Update `docs/RUNTIME_HOME.md` (event-log retention) and
  `docs/RUNTIME_LOGGING_AND_EVALUATION.md` (logging-gap closure) at implementation.

## 8. CLI / config / operator surface deltas

- No new CLI subcommand is required for Direction A or B; the bound is a config
  setting applied by the worker at startup/rotation. UI is out of scope
  (`docs/MEASUREMENT_GATE.md`).
- Doc updates at implementation are `docs/RUNTIME_HOME.md` and
  `docs/RUNTIME_LOGGING_AND_EVALUATION.md` per `AGENTS.md` §Change Checklist.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| `docs/event-log-retention-decision-2026-06-18.md` | Retention model and bound: **A+B accepted** — rotate the event JSONL on the `log_rotate_hours`/`log_retain_days` window AND reduce routine `control_loop.write_applied` persistence, always keeping `warning`+ and lifecycle events; single atomic append preserved; absent-key default = current behavior. | Accepted 2026-06-18 (current) |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-EVENTRET-01 | T, R | `.\scripts\Test-LocalCI.ps1`: a test that writes past the accepted bound and asserts the active event JSONL is rotated/capped (or routine `info` is reduced); review vs the design decision recording the model and bound. |
| REQ-EVENTRET-02 | T, R | Test asserts concurrent appenders produce only whole NDJSON lines across a rotation boundary (no split/interleaved line); review of the atomic-append + rotation handling vs `runtime_event_log.cpp:28-40,299-321` and the torn-write finding. |
| REQ-EVENTRET-03 | T | Test asserts a `warning`/`error` event is retained while routine `info` `write_applied` is reduced/rotated out within the accepted window. |
| REQ-EVENTRET-04 | T, R | Test asserts an absent config key preserves current append behavior and that an existing event JSONL plus `CachedEventCount` still parse; review vs `docs/RUNTIME_HOME.md` schema stability. |
| REQ-EVENTRET-05 | R | Review vs `docs/CONTROL_PIPELINE_MATH.md` / `docs/MEASUREMENT_GATE.md`: computed duty/cadence/channels/identity and CSV retention unchanged; event logging remains best-effort and does not change control decisions; docs updated. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Accepted decisions and follow-on choices

| Decision | Needed before | Current default |
|---|---|---|
| Whether to bound the event log at all, or accept the current append-only behavior. | promotion | Resolved: bound it. The unbounded event JSONL is material runtime disk growth. |
| Retention model: Direction A (age rotation), Direction B (severity-based persistence), or a combination. | implementation | Resolved: A+B. Rotate by age and reduce routine `write_applied` persistence while keeping diagnostics. |
| Whether `control_loop.write_applied` is dropped, sampled, or kept and only rotated. | implementation | Resolved: sampled in the control loop (tick 1 plus every 240th tick by default); CSV fidelity is unchanged. |
| New event-specific config keys vs. reusing `log_rotate_hours` / `log_retain_days`. | implementation | Resolved: reuse `log_rotate_hours` / `log_retain_days`; no new config key. |
| The mid-file NUL torn-write cause (crash mid-write vs. non-atomic flush vs. concurrent writer). | implementation | Bounded for this change: persisted events remain single-call appends, rotation is in-process serialized, and the test parses every line across concurrent appends at a rotation boundary. |

## 12. Measurement gate & dependencies

- **Measurement gate:** not crossed. This is logging-side retention only; it does
  not change cadence, live channels, or mixed-input strategy and adds no control
  term, so no characterization evidence is required before a decision
  (`docs/MEASUREMENT_GATE.md`).
- **Depends on:** the event-log append path (`src/runtime/runtime_event_log.cpp`)
  and the artifact naming / path resolution (`src/runtime/runtime_paths.cpp`,
  `EvidenceLogArtifactNaming`). Independent of FEAT-0016 (analyze DB run-purge);
  the two together close the two unbounded artifacts in issue #4 but touch
  different code and can land separately.
- **Build/test impact:** implemented tests under `tests/cpp/runtime_event_log_tests.cpp`
  (rotation/prune, line atomicity across rotation, severity retention/sampling,
  backward-compatible default behavior via unconfigured append path) plus docs
  updates to `docs/RUNTIME_HOME.md` and
  `docs/RUNTIME_LOGGING_AND_EVALUATION.md`. No
  `docs/CONTROL_PIPELINE_MATH.md` change.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2 — runtime-observed in `docs/discovery-runtime-disk-growth-2026-06-14.md` Finding 1 and static-verified at `runtime_event_log.cpp:210`).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [x] 3. Required design decision record(s) written and marked current (§9 — `docs/event-log-retention-decision-2026-06-18.md`, Accepted 2026-06-18).
- [x] 4. Concrete `REQ-EVENTRET-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — `Test-LocalCI`, contract review (§10), to be mirrored in `docs/TRACEABILITY.md` on acceptance.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline (logging-side only; computed duty unchanged; additive schema).
- [x] 7. Doctrine check: current behavior claims grounded with file:line; proposed behavior labeled as proposed; `must`/`should`/`is` used per `CLAUDE.md`; no undefined terms or unqualified vague adjectives.

> Promoted to Accepted 2026-06-18 and implemented the same day after maintainer
> authorization: the A+B retention model (age rotation + severity-aware reduction
> of `write_applied`) is built and verified. Run
> `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` for the maintained gate.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-EVENTRET-01 | pass | `tests/cpp/runtime_event_log_tests.cpp::TestRotationUsesActiveStartAndPrunesArchives` rotates an aged active event JSONL with a fresh last-write mtime, prunes an expired event archive, and keeps the new active event; `TestWriteAppliedReductionPreservesDiagnostics` verifies routine `write_applied` sampling. Full `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` passed. | 2026-06-18 |
| REQ-EVENTRET-02 | pass | `TestConcurrentAppendRotationKeepsWholeLines` appends concurrently across a rotation boundary, then parses every active/archive line as JSON; implementation review confirms single-call append and in-process rotation/append serialization. | 2026-06-18 |
| REQ-EVENTRET-03 | pass | `TestWriteAppliedReductionPreservesDiagnostics` drops routine `write_applied` ticks 2/4/5 while retaining tick 1, tick 3, `control_loop.write_failed`, and `control_loop.shutdown`. | 2026-06-18 |
| REQ-EVENTRET-04 | pass | The event payload schema stays `svg_mb_control.event.v1`; unconfigured append paths keep historical behavior, and `CachedEventCount` remains the active-file count source. Runtime event-log tests and full `Test-LocalCI` passed. | 2026-06-18 |
| REQ-EVENTRET-05 | pass | Review: change is confined to event-log management/registration and `control_loop.write_applied` persistence sampling; computed duty, cadence, channel set, control identity, and CSV archive retention are unchanged. `docs/RUNTIME_HOME.md` and `docs/RUNTIME_LOGGING_AND_EVALUATION.md` updated. | 2026-06-18 |

**Spec vs. implementation deltas:** Implemented the accepted A+B direction with
age-based rotation, not size-based rotation. Reused existing
`log_rotate_hours` / `log_retain_days` rather than adding event-specific config.
The routine `control_loop.write_applied` policy is sample-first (tick 1 and every
240th tick by default), not drop-all. The torn-write cause is not root-caused,
but the implementation bounds this feature's risk by retaining single-call append
and adding a whole-line rotation/concurrency test. Rotation is keyed to the
registered active-start time, not file last-write mtime, so steady appends do not
postpone the configured rotation deadline.
