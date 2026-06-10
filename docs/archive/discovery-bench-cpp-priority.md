# Discovery — Bench and Control C++ Priority

**Goal:** Assess what it would take to make logging a clean C++-owned priority, then reduce Python where it is not close to essential, using the existing Bench and Control repos as the baseline.
**Date:** 2026-04-16
**Status:** complete
**Recommended next:** historical only. The original next step was to plan a
shared native logging cleanup across Bench and Control; Control's current
runtime/evidence logging work has since advanced enough that new planning should
start from `docs\RUNTIME_LOGGING_AND_EVALUATION.md` instead.

> Historical note, 2026-05-21: Control now owns a richer runtime/evidence
> logging plane and analyzer path. This document remains useful for the
> native-first rationale, but its Control cleanup recommendations are older than
> the current runtime logging docs.

---

## Questions

1. How C++-first is `SVG-MB-Control` already?
2. What native logging machinery already exists in `SVG-MB-Bench` that Control could borrow?
3. What still keeps Control's logger from being "clean"?
4. Which Python surfaces in Bench are close to essential, and which are mostly wrapper or analysis glue?
5. How much script surface actually exists in Control versus Bench?
6. What scope tiers best fit the requested priorities?

---

## Findings

### Q1: How C++-first is `SVG-MB-Control` already?

**Answer:** Control is already very close to the desired end state. The shipping runtime is native, direct-only, and its long-running modes already log in-process through C++. Python is only used for the smoke suite, not for runtime behavior.

**Evidence:**
- `README.md:3` - describes `SVG-MB-Control` as the standalone runtime repo.
- `README.md:12` - says `svg-mb-control.exe` is the only runtime executable.
- `src/main.cpp:204` - rejects legacy bridge options and states this branch is direct-only.
- `src/control_config.cpp:263-264` - rejects the old `logger_service_duration_ms` bridge-era config field.
- `README.md:151-152` - documents `logs\svg_mb_control_output.csv` and `logs\svg_mb_control_events.jsonl`.
- `README.md:166` - the only documented Python lane is `python -m unittest discover tests -v`.

**Implications:**
- Control does not need a language migration.
- The work is a cleanup and consolidation pass, not a rewrite.

### Q2: What native logging machinery already exists in `SVG-MB-Bench` that Control could borrow?

**Answer:** Bench already has the cleaner native logging substrate. It has a documented artifact contract, native CSV and JSONL writers, manifest accounting, live write-through latest files, and a deadline scheduler for rate-controlled capture.

**Evidence:**
- `../SVG-MB-Bench/README.md:24-25` - Bench is explicitly the measurement lane for Control cadence work.
- `../SVG-MB-Bench/README.md:272-280` - `logger-service` is already a persistent native logger with rotating archive segments, `current_state.json`, live CSV and JSONL latest files, and finalized manifest accounting.
- `../SVG-MB-Bench/docs/LOG_ARTIFACT_CONTRACT.md:7` - defines compatible archive naming plus explicit `rows_written` and `events_written`.
- `../SVG-MB-Bench/docs/LOG_ARTIFACT_CONTRACT.md:288-290` - documents the live logger behavior and exact latest-file replacement semantics.
- `../SVG-MB-Bench/src/artifact_writers.h:20-42` and `../SVG-MB-Bench/src/artifact_writers.cpp:74-212` - implement native CSV and JSONL writers with write-through latest copies and finalize/discard semantics.
- `../SVG-MB-Bench/src/session_manifest.cpp:138-141` and `../SVG-MB-Bench/src/session_manifest.cpp:207-208` - finalize and persist `rows_written` and `events_written`.
- `../SVG-MB-Bench/src/deadline_scheduler.h:20-27` and `../SVG-MB-Bench/src/deadline_scheduler.cpp:77-133` - provide deadline-based waiting with skip accounting instead of simple fixed-delay sleeping.
- `../SVG-MB-Bench/src/logger_service.cpp:592-597` - wires the logger to native CSV and JSONL writers in write-through mode.
- `../SVG-MB-Bench/src/logger_service.cpp:688` and `../SVG-MB-Bench/src/logger_service.cpp:883` - uses the native deadline scheduler in the logger loop.

**Implications:**
- The best path is not inventing a new logger from scratch in Control.
- The best path is to lift or mirror Bench's native logging primitives into a shared Control-appropriate core.

### Q3: What still keeps Control's logger from being "clean"?

**Answer:** Control has the right native direction, but it is still internally inconsistent. Logging exists, yet error reporting, time formatting, JSON escaping, and config parsing are still duplicated across modules, and write/reconcile flows still split between structured events and raw `stderr`.

**Evidence:**
- `src/runtime_logging.cpp:321-456` - Control already has a native CSV logger with rotation and live mirror writes.
- `src/runtime_logging.cpp:563-653` - Control already has native JSONL event appends.
- `src/write_orchestrator.cpp:110`, `:145`, `:165`, `:180`, `:196`, `:230`, `:249`, `:304`, `:349`, `:372`, `:392`, `:411`, `:438` - many failures still emit directly to `std::cerr`.
- `src/read_loop.cpp:46` and `src/control_loop.cpp:221` and `src/runtime_logging.cpp:18` and `src/write_orchestrator.cpp:42` and `src/direct_runtime_snapshot.cpp:63` - `FormatLocalIso8601` is duplicated across multiple translation units.
- `src/read_loop.cpp:61` and `src/control_loop.cpp:231` and `src/runtime_logging.cpp:150` and `src/pending_writes.cpp:113` and `src/runtime_snapshot.cpp:131` - `JsonEscape` is duplicated across multiple translation units.
- `src/control_config.cpp:102-131` - the base config loader uses its own hand-rolled JSON parsing helpers.
- `src/control_loop.cpp:52-178` - the control loop has a second hand-rolled JSON scanner just for `control_loop`.
- `src/runtime_write_policy.cpp:38-94` - runtime policy parsing has a third copy of object and array scanning helpers.

**Implications:**
- A "clean logger" pass in Control is mostly a consolidation pass:
- one shared time utility,
- one shared JSON writer or tiny native serialization layer,
- one log/event/reporting surface for all runtime modes,
- optionally one manifest/accounting layer if you want Bench-grade artifact discipline.

### Q4: Which Python surfaces in Bench are close to essential, and which are mostly wrapper or analysis glue?

**Answer:** Most Python in Bench is not runtime-essential. The big exceptions are the offline SQLite ingestion and reporting utilities, which are useful but still not part of the shipping runtime. The lowest-value Python is the generic `python -m scripts` dispatcher and the small utility wrappers around tests or audits.

**Evidence:**
- `../SVG-MB-Bench/README.md:67` and `:73` - the public test and CLI wrapper entrypoints are `python -m scripts ...`.
- `../SVG-MB-Bench/scripts/core/entrypoint.py:26-126` - the Python dispatcher mostly forwards commands to either PowerShell scripts or other Python scripts.
- `../SVG-MB-Bench/scripts/tools/run_tests.py:1-44` - the test runner is a thin wrapper over `unittest`.
- `../SVG-MB-Bench/scripts/tools/summary_audit.py:1-177` - summary audit is standalone repo-maintenance logic, not runtime logic.
- `../SVG-MB-Bench/scripts/tools/validation_plan.py:1-220` and `:1261` - validation orchestration is large, but it is a workflow layer over existing native and PowerShell commands.
- `../SVG-MB-Bench/scripts/ingest_logs.py:3-18` - log ingestion is an offline SQLite analysis tool.
- `../SVG-MB-Bench/scripts/query_thermal.py:3-12` and `:933` - query/reporting is an offline database consumer layered on top of ingestion.

**Implications:**
- Easy removals:
- the Python dispatcher,
- the Python unittest runner,
- the Python summary audit wrapper.
- Medium candidates for native or PowerShell replacement:
- validation plan orchestration.
- Harder candidates that only make sense if you really want a native analysis stack:
- SQLite ingestion and query/reporting.

### Q5: How much script surface actually exists in Control versus Bench?

**Answer:** Control already has very little Python. Bench still has a large script surface, mostly PowerShell plus a smaller but still substantial Python layer.

**Evidence:**
- 2026-04-16 repo count command, excluding generated directories in Control, measured:
  - `.cpp`: 25 files / 9084 lines
  - `.h`: 37 files / 1858 lines
  - `.py`: 2 files / 901 lines
  - `.ps1`: 2 files / 919 lines
- 2026-04-16 repo count command, excluding generated directories in Bench, measured:
  - `.cpp`: 36 files / 14110 lines
  - `.h`: 48 files / 1673 lines
  - `.py`: 15 files / 5277 lines
  - `.ps1`: 40 files / 12387 lines
- 2026-04-16 Bench script count command measured the largest Python files as:
  - `scripts/ingest_logs.py` - 1529 lines
  - `scripts/tools/validation_plan.py` - 1139 lines
  - `scripts/query_thermal.py` - 848 lines
  - `scripts/core/entrypoint.py` - 238 lines

**Implications:**
- "Reduce Python" is basically already done in Control.
- "Reduce Python" is a real repo-shape project in Bench.
- If the target is both repos, Control can be cleaned quickly while Bench will take materially longer.

### Q6: What scope tiers best fit the requested priorities?

**Answer:** There are three realistic scope tiers.

**Tier 1 - Control cleanup only**

Make Control's current logger coherent without changing Bench:
- route `write-once` and `reconcile` failures through one native reporting layer instead of mixed `AppendRuntimeEvent` plus `std::cerr`
- centralize time formatting and JSON escaping
- centralize config/runtime-policy/control-loop parsing helpers
- add a small timing block to Control logs if cadence tuning is still a goal

This is the shortest path to a clean C++ logger in the shipping runtime.

**Tier 2 - Shared native logging core**

Promote Bench's native logging pieces into a reusable core and make Control use them:
- artifact writers
- manifest/accounting
- current-state publishing helpers
- deadline scheduler where high-rate cadence matters

This creates the cleanest architecture, but it is a cross-repo refactor rather than a local cleanup.

**Tier 3 - Bench script reduction**

Reduce Python in Bench in descending order of payoff:
- remove the Python command dispatcher first
- replace the Python test runner and summary audit next
- decide whether validation orchestration should be PowerShell-only or native
- only then decide whether SQLite ingestion and query should become native C++

**Evidence:**
- Control already owns native runtime logging: `src/runtime_logging.cpp:321-456`, `src/runtime_logging.cpp:563-653`.
- Bench already owns the richer native logging primitives: `../SVG-MB-Bench/src/artifact_writers.cpp:74-212`, `../SVG-MB-Bench/src/logger_service.cpp:592-597`, `../SVG-MB-Bench/src/logger_service.cpp:688`, `../SVG-MB-Bench/src/logger_service.cpp:883`.
- Bench's Python surface is still mostly tooling/orchestration: `../SVG-MB-Bench/scripts/core/entrypoint.py:26-126`, `../SVG-MB-Bench/scripts/tools/run_tests.py:1-44`, `../SVG-MB-Bench/scripts/tools/summary_audit.py:1-177`, `../SVG-MB-Bench/scripts/tools/validation_plan.py:1-220`.

**Implications:**
- If the real target is the shipping runtime, do Tier 1 first.
- If the target is repo hygiene across the workspace, do Tier 2, then Tier 3.

---

## Cross-Cutting Analysis

### Constraints

- Control is already direct-only and should not regress back toward bridge-era process splits.
- Bench is intentionally the measurement lane, so some workflow scripting may still be justified there even after cleanup.
- Bench's logger is already native; the main architectural choice is whether to extract shared code or tolerate duplication.
- Replacing SQLite-oriented Python analysis with C++ is possible, but it is materially more expensive than removing wrapper scripts.

### Risks

| Risk | Likelihood | Impact | Notes |
|------|-----------|--------|-------|
| Cleaning only Control and ignoring Bench duplication | M | M | Fastest short-term path, but shared logging concepts will continue to drift. |
| Trying to remove all Python from Bench in one shot | H | H | The offline analysis utilities are large enough to turn this into a side project. |
| Keeping mixed `std::cerr` plus structured event logging in Control | H | M | The runtime remains native, but operator diagnostics stay inconsistent. |
| Porting Bench analysis tools before deciding whether they are still needed | M | H | Native rewrites of `ingest_logs.py` and `query_thermal.py` are expensive if the workflow is not final. |

### Open Questions

- Whether you want Control only cleaned, or whether you want Bench and Control to share one logging substrate.
- Whether Bench's offline SQLite ingestion/query lane is worth keeping at all, versus retiring it and staying file-first.

---

## Recommendation

Proceed in this order:

1. Clean Control first. It is already close, and the work is mostly deduplication and log-surface unification.
2. If you still want one logger story across the workspace, extract Bench's native writer, manifest, and scheduler pieces into shared C++ and move Control onto them.
3. Reduce Python in Bench in phases, starting with the low-value wrapper layer:
   - remove `python -m scripts` dispatch
   - replace `run_tests.py` and `summary_audit.py`
   - decide whether `validation_plan.py` should remain orchestration or move native
   - only then decide whether `ingest_logs.py` and `query_thermal.py` are worth a C++ rewrite

Rough effort:

- Control-only logger cleanup: about 2-4 focused days.
- Shared native logging core across Bench and Control: about 1-2 weeks.
- Bench Python reduction, excluding ingestion/query rewrites: about 3-6 days.
- Native replacement of Bench ingestion/query tooling: about 1-2 additional weeks, depending on feature parity target.

---

## Appendix

### Repo count commands run on 2026-04-16

Control, excluding generated directories:

- `.cpp`: 25 files / 9084 lines
- `.h`: 37 files / 1858 lines
- `.py`: 2 files / 901 lines
- `.ps1`: 2 files / 919 lines

Bench, excluding generated directories:

- `.cpp`: 36 files / 14110 lines
- `.h`: 48 files / 1673 lines
- `.py`: 15 files / 5277 lines
- `.ps1`: 40 files / 12387 lines

Bench Python file sizes:

- `scripts/ingest_logs.py` - 1529 lines
- `scripts/tools/validation_plan.py` - 1139 lines
- `scripts/query_thermal.py` - 848 lines
- `scripts/core/entrypoint.py` - 238 lines
- `scripts/tools/summary_audit.py` - 177 lines
- `scripts/tools/run_tests.py` - 44 lines
