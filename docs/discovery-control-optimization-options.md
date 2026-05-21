# Discovery - Control Optimization Options

**Goal:** Evaluate `SVG-MB-Control` for additional optimization and code-quality
opportunities after CSV flush tuning.
**Date:** 2026-05-19
**Status:** complete; partly superseded by the 2026-05-21 runtime/analyzer pass
**Recommended next:** treat the optimization register as historical. Runtime
event counts, direct-runtime simulation caching, and major control-loop splitting
have moved since this document was written. The current remaining structural
candidate is a smaller `RunUntilStopped()` extraction after response tuning has a
baseline.

> Historical note, 2026-05-21: file/line anchors and some hot-path candidates
> below predate the runtime module split and analyzer/logging commit. Treat the
> optimization register's updated decisions as the current reading.

---

## Questions

1. What work still runs every control tick, and which pieces look like avoidable
   hot-path cost?
2. Where does runtime logging still do synchronous or repeated I/O after the CSV
   flush change?
3. Which parts of `ControlLoop::RunUntilStopped()` are still high-complexity
   extraction candidates?
4. What test or verification coverage already exists around those candidates?
5. Which candidates are low-risk enough to implement next, and which need
   measurement first?

---

## Findings

### Q1: What work still runs every control tick?

**Answer:** The loop is already throttling the expensive publication surfaces,
but each tick still samples the direct runtime, scans per-channel state, builds a
wide CSV row, samples process resources, and performs several fixed-size linear
lookups. The biggest low-risk hot-path cleanup is caching simulation environment
overrides; the biggest medium-risk cleanup is avoiding per-row timestamp parsing
and repeated vector scans.

**Evidence:**
- `src/control_loop.cpp:534` - the steady loop starts and checks the runtime stop
  file every tick.
- `src/control_loop.cpp:550` - each tick calls `SampleDirectRuntimeSnapshot`.
- `src/control_loop.cpp:609` - each tick iterates configured channels and handles
  restore, evaluate, write gating, sidecar persistence, and write events.
- `src/control_loop.cpp:900` - each tick calls `SampleProcessResources`, while
  CPU percentage is only recomputed when the resource window reaches about one
  second at `src/control_loop.cpp:913`.
- `src/control_loop.cpp:945` - each tick builds and writes the control-loop CSV
  row.
- `src/direct_runtime_snapshot.cpp:19` and `src/direct_runtime_snapshot.cpp:22`
  - simulation helpers read environment variables with `_dupenv_s`.
- `src/direct_runtime_snapshot.cpp:94`, `src/direct_runtime_snapshot.cpp:97`,
  `src/direct_runtime_snapshot.cpp:100`, `src/direct_runtime_snapshot.cpp:137`,
  and `src/direct_runtime_snapshot.cpp:145` - five simulation override checks
  happen during snapshot sampling.
- `src/runtime_logging.cpp:17` and `src/runtime_logging.cpp:148` - CSV row
  assembly parses `snapshot_time_iso` back into a time point to calculate
  snapshot age, even though the direct sampler creates the timestamp at
  `src/direct_runtime_snapshot.cpp:139`.
- `src/runtime_snapshot.cpp:137` - fan lookup is a linear scan; the CSV writer
  calls this for each fixed log channel at `src/runtime_logging.cpp:178`.

**Implications:**
- Caching simulation overrides is low-risk because tests set the relevant
  environment before process launch.
- Moving snapshot age to the runtime model is cleaner, but it touches snapshot
  creation, serialization assumptions, and CSV formatting.
- Indexed fan/channel views are probably not urgent because the current fan log
  channel count is fixed at seven, but they would make hot-path code less
  lookup-heavy.

### Q2: Where does runtime logging still do repeated synchronous I/O?

**Answer:** CSV row flushing is now configurable, but manifest and event I/O can
still become nonlinear over long runs. The main issue is that every manifest
update recounts the whole JSONL event log. Event writes also open the event file
for each event, which is acceptable for sparse events but worth measuring if
write-applied events become frequent.

**Evidence:**
- `src/runtime_artifacts.cpp:25` - runtime manifests update every 100 CSV rows.
- `src/runtime_artifacts.cpp:39` - `CountNonEmptyLines` scans the event log from
  the start.
- `src/runtime_artifacts.cpp:240` - every manifest write calls
  `CountNonEmptyLines`.
- `src/runtime_artifacts.cpp:312` and `src/runtime_artifacts.cpp:313` - each
  manifest update writes both archive and latest manifests.
- `src/json_io.cpp:73` through `src/json_io.cpp:107` - atomic JSON writes flush a
  temp file and use `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` on Windows.
- `src/runtime_artifacts.cpp:443` through `src/runtime_artifacts.cpp:454` -
  every runtime event recreates the parent directory check and opens the JSONL
  file in append mode.
- `src/control_loop.cpp:849` - successful writes append a
  `control_loop.write_applied` event.
- `src/pending_writes.cpp:136` through `src/pending_writes.cpp:140` - pending
  write upserts intentionally persist synchronously before `ApplyDuty`, so that
  cost is part of the crash-recovery contract rather than an easy optimization.

**Implications:**
- Replace event-log recounting before trying broader logging changes.
- Keep `PendingWritesStore::Upsert` synchronous unless the recovery contract is
  redesigned and tested.
- A persistent `RuntimeEventLogger` could reduce event open/close churn, but the
  event rate should be measured first.

### Q3: Which control-loop areas are still extraction candidates?

**Answer:** `RunUntilStopped()` is still the primary code-quality hotspot. The
remaining valuable extractions are hold restore handling, write eligibility and
write application, end-of-tick publication, and shutdown restore handling.

**Evidence:**
- `src/control_loop.cpp` is 1098 lines.
- `src/control_loop.cpp:616` through `src/control_loop.cpp:672` - active hold
  restore handling mixes fan restore, event logging, sidecar removal, and fatal
  timeout decisions.
- `src/control_loop.cpp:682` through `src/control_loop.cpp:862` - write
  eligibility, pending sidecar upsert, fan write application, circuit breaker
  handling, authority events, and success bookkeeping are one inline block.
- `src/control_loop.cpp:883` through `src/control_loop.cpp:989` - timing,
  resource sampling, abort handling, CSV row publication, low-band evidence, and
  status publication are grouped at the end of each tick.
- `src/control_loop.cpp:1009` through `src/control_loop.cpp:1074` - shutdown
  restore and sidecar cleanup duplicate some restore/event concerns from the
  steady-state loop.

**Implications:**
- The next extraction should be behavior-preserving and test-backed:
  `HandleExpiredHoldRestore`, `TryApplyChannelSetpoint`, and
  `PublishControlTickArtifacts` are natural seams.
- Extracting the whole loop into a state machine is not justified by the current
  performance profile; smaller helpers reduce risk.

### Q4: What coverage exists?

**Answer:** The control loop has good behavioral coverage for writes, timing
fields, policy refusal, restore timeout, low-band behavior, and runtime
artifacts. Coverage is weaker around long-run manifest scalability and around
code-quality-only refactors.

**Evidence:**
- `tests/test_control_loop.py:13` - the main control-loop smoke verifies 50 ms
  tick configuration, writes, status fields, CSV fields, and timing/resource
  publication.
- `tests/test_control_loop.py:632` - policy refusal coverage asserts pending
  writes are flushed after refused writes.
- `tests/test_control_loop.py:671` - restore timeout coverage verifies abort
  behavior and sidecar preservation.
- `tests/test_read_loop.py:130` and `tests/test_read_loop.py:165` - read-loop
  manifest coverage now verifies non-default CSV flush policy metadata.
- `tests/test_config_contracts.py:27` through `tests/test_config_contracts.py:34`
  - shipped config tests currently assert 250 ms poll/write cadence.

**Implications:**
- Refactoring restore/write blocks is feasible because existing tests cover the
  key external behavior.
- Manifest event-count optimization needs a focused test that appends events and
  checks manifest counts without relying on a full long-running profile.
- There is a documentation/config drift that should be resolved before cadence
  decisions are treated as final.

### Q5: Which candidates are next?

**Answer:** Implement event-count optimization and simulation override caching
next. Then do a focused control-loop helper extraction. Defer deeper data-model
changes and persistent event logging until after a measured run shows they
matter.

**Evidence:**
- `docs/MEASUREMENT_GATE.md:5` through `docs/MEASUREMENT_GATE.md:12` says the
  current packaged profile is 50 ms.
- `docs/RUNTIME_LOGGING_AND_EVALUATION.md:17` through
  `docs/RUNTIME_LOGGING_AND_EVALUATION.md:26` records 50 ms evidence with no
  overrun rows and process CPU around 0.207%-0.29%.
- `config/control.release.json:15` and `config/control.release.json:16` set the
  current shipped config to 250 ms poll/write cadence.
- `tests/test_config_contracts.py:27` through `tests/test_config_contracts.py:34`
  assert the 250 ms shipped config.

**Implications:**
- Treat the runtime profile as a release-contract decision: either update docs
  to 250 ms or restore configs/tests to 50 ms. Optimization work should not
  silently assume one while the repo encodes the other.

---

## Optimization Register

| Candidate | Type | Evidence | Risk | Confidence | Decision |
|-----------|------|----------|------|------------|----------|
| Maintain runtime event counts instead of scanning JSONL on every manifest write | hot-path I/O | now owned by `RuntimeEventLog` and `RuntimeCsvLogger` | Medium | High | implemented |
| Cache direct-runtime simulation environment overrides once per process | hot-path/code quality | direct-runtime snapshot simulation env handling was cleaned up after this note | Low | High | implemented |
| Extract hold-restore and write-apply helpers from `RunUntilStopped()` | structural | `tick_runner` and related helpers reduced the old control-loop body | Medium | High | partly done; defer remaining extraction |
| Resolve 50 ms documentation vs 250 ms shipped config drift | correctness/code quality | shipped configs/tests now assert the 250 ms profile while docs record measurement gates separately | Medium | High | resolved as shipped 250 ms profile |
| Avoid per-row parsing of `snapshot_time_iso` by carrying time/age in `RuntimeSnapshot` | hot-path allocation/parsing | snapshot timing/age fields were widened after this note | Medium | Medium | partly done |
| Build indexed fan/channel views for CSV/control decisions | hot-path/structure | current direct snapshot/control row path still uses small fixed-size scans | Low | Medium | defer |
| Sample process memory/resources on a one-second cadence instead of every tick | hot-path WinAPI | resource sampling is already windowed for CPU percentage | Medium | Medium | suggest-only |
| Add a persistent runtime event logger | I/O | runtime JSONL event writer and classification are implemented | Medium | Medium | implemented |

## Baseline

- Existing measured profile: docs record 50 ms runs with no overrun rows and
  average process CPU around 0.207%-0.29%.
- Current repo config state: shipped configs and config-contract tests encode
  250 ms poll/write cadence.
- Verification path: `.\build-release.ps1 -NoPublish -NoStopProcesses` exercises
  the supported build path and hermetic test lane.
- New profiling was not run during this discovery; recommendations above are
  based on code inspection and existing documented measurements.

---

## Cross-Cutting Analysis

### Constraints

- `PendingWritesStore::Upsert` must remain synchronous unless the crash-recovery
  contract changes.
- Full per-tick CSV remains part of the measurement gate; do not suppress fields
  before a replacement summary path exists.
- The repo currently has a cadence contract mismatch: docs describe 50 ms while
  configs/tests assert 250 ms.

### Risks

| Risk | Likelihood | Impact | Notes |
|------|------------|--------|-------|
| Event-count optimization undercounts external JSONL appends | Medium | Medium | If other writers append events, a cached count needs reconciliation on open or terminal manifest. |
| Control-loop extraction changes sidecar persistence ordering | Medium | High | Preserve upsert-before-write and policy-refusal cleanup ordering. |
| Caching simulation env vars breaks tests that mutate env mid-process | Low | Medium | Current tests spawn processes with env already set. |
| Cadence drift causes wrong optimization target | High | Medium | Resolve 50 ms vs 250 ms before new profile conclusions. |

### Open Questions

- Is the current shipped cadence intentionally 250 ms, or should configs/tests be
  returned to the documented 50 ms profile?
- How large do JSONL event files get during normal multi-hour control runs after
  CSV flush batching?

---

## Recommendation

Historical recommendation, now mostly implemented:

1. Done: replace repeated manifest event-log recounting with runtime event
   accounting.
2. Done: clean up simulation-only environment override handling in direct
   runtime sampling.
3. Partly done: split the old control-loop body into `tick_runner` and related
   helpers while preserving sidecar ordering.
4. Done: treat the packaged 250 ms cadence as the shipped profile; use measured
   run summaries before changing it again.

Current next step: use analyzer-backed run data before any further hot-path
optimization or control-loop extraction.
