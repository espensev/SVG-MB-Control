# Discovery — Current State Vs Earlier

> Status, 2026-05-26: historical snapshot. This 2026-04-17 review predates
> the native logger, runtime sidecars (`control_runtime.json`,
> `control_supervisor.json`, `control_health.json`), the rotated CSV /
> JSONL event log under `runtime\logs\`, the manifest-driven analyzer
> workflow, and the 250 ms shipped control profile. Use
> `docs\RUNTIME_LOGGING_AND_EVALUATION.md` and `docs\RUNTIME_HOME.md` for
> the current logging and runtime-home design.

**Goal:** Evaluate the current state of the Control and Bench repos versus the earlier logger / native-first assessment.
**Date:** 2026-04-17
**Status:** complete (superseded; see banner above)
**Recommended next:** none — standalone research

---

## Questions

1. Has `SVG-MB-Control` materially advanced toward a clean native logger since the earlier assessment?
2. Has `SVG-MB-Bench` materially advanced its native logger architecture since the earlier assessment?
3. Has the Bench helper surface actually reduced Python / script dependence?
4. What does the current delta imply for priority 1 and priority 2?

---

## Findings

### Q1: Has `SVG-MB-Control` materially advanced toward a clean native logger since the earlier assessment?

**Answer:** Partially, but not enough to change the main conclusion. Control is still direct-only and still has native CSV/JSONL logging, but the runtime reporting surface is not yet clean because error paths still mix structured runtime events with ad hoc `std::cerr` output and duplicated helper code.

**Evidence:**
- `README.md:132` — Control still documents rejection of bridge-era keys including `logger_service_duration_ms`.
- `README.md:151-158` — Control still presents the native runtime-home outputs, including `logs\svg_mb_control_output.csv` and `logs\svg_mb_control_events.jsonl`.
- `src/control_config.cpp:250-270` — config load still hard-fails legacy bridge-era fields instead of depending on Bench logger settings.
- `src/runtime_logging.cpp:563-653` — Control has a native JSONL event logger.
- `src/write_orchestrator.cpp:101-438` — write/reconcile code still mixes `AppendRuntimeEvent(...)` with many direct `std::cerr` failures and warnings.
- `src/main.cpp:304` and `src/main.cpp:392` — main still emits direct `std::cerr` error text.
- `src/read_loop.cpp:46-61`, `src/control_loop.cpp:221-231`, `src/runtime_logging.cpp:18-150`, `src/write_orchestrator.cpp:42`, `src/direct_runtime_snapshot.cpp:63` — shared formatting/escaping helpers remain duplicated.

**Implications:**
- Priority 1 is still unfinished in Control.
- The remaining work is cleanup / consolidation work, not a greenfield logger build.
- The Control runtime is native-first already; the gap is consistency and ownership of diagnostics.

### Q2: Has `SVG-MB-Bench` materially advanced its native logger architecture since the earlier assessment?

**Answer:** Yes. Bench has moved from a mostly monolithic `logger-service` implementation to a more decomposed native logging stack with clearer ownership split across config, core loop, segment lifecycle, capture/state, and summary concerns.

**Evidence:**
- `src/logger_service.cpp:1-147` — current entrypoint is now thin and delegates into `parse_logger_service_options(...)`, `build_logger_service_config(...)`, and `run_logger_core(...)`.
- `src/logger_core.cpp:49-299` — capture scheduling, backend init, row accumulation, snapshot publishing, and rollover decisions now live in a separate core loop.
- `src/logger_segment.cpp:74-223` — archive-path, manifest, CSV header, and segment-open logic are separated into their own unit.
- `src/logger_lifecycle.cpp:8-139` — rollover/finalize/failure/terminal-state publication is isolated into lifecycle helpers.
- `README.md:322-339` — Bench now documents the persistent read-only logger with live latest-copy CSV/JSONL plus manifest/current-state behavior.
- `tests/native/live_tests.cpp:631-689` — native tests assert the logger-service contract and latest-surface behavior.
- `tests/test_bridge_contract_live.py:258-308` — Python-side contract tests still validate the same logger-service shape.

**Implications:**
- Bench is now a stronger donor for logger structure than it was earlier.
- If you want a cleaner Control logger, reusing Bench’s decomposition patterns is more realistic now than during the earlier pass.
- Bench is still a proof seam, not the shipping runtime.

### Q3: Has the Bench helper surface actually reduced Python / script dependence?

**Answer:** Not materially. The helper surface has shifted, but it has not been simplified in the way your priority 2 asks for. Python dispatch still exists, many workflows still terminate in PowerShell, and some new helpers were added rather than removed.

**Evidence:**
- `scripts/core/entrypoint.py:18-121` — `python -m scripts` still fronts the helper command table.
- `scripts/core/entrypoint.py:187-229` — dispatch still routes either to Python modules or PowerShell scripts.
- `scripts/README.md:50-64` — the documented workflow map still points most higher-level tasks at PowerShell runners.
- `scripts/tools/run_tests.py` and `scripts/tools/summary_audit.py` still exist and remain first-class helper commands.
- `scripts/tools/native_cli_contract.py` is new, but it is still a Python wrapper over native `ctest`.
- `git status --short` in Bench shows:
  - `D scripts/Test-CliContract.ps1`
  - `?? scripts/Run-ControlCharacterization.ps1`
  - `?? scripts/Run-GpuRateSweep.ps1`
  - `?? scripts/Run-LoggerServiceSweep.ps1`
  - `?? scripts/tools/native_cli_contract.py`

**Implications:**
- Priority 2 has not really happened yet.
- Some script ownership may have improved, but overall there is still a Python/PowerShell orchestration layer around the native binary.
- The easiest wins are still the same small helpers: test aggregation, audit tooling, and dispatch glue.

### Q4: What does the current delta imply for priority 1 and priority 2?

**Answer:** Priority 1 is closer, because Bench’s native logger has matured structurally, but Control still needs the same final cleanup work. Priority 2 is not closer in outcome terms; Bench still relies heavily on wrapper scripts even if some specific helpers changed.

**Evidence:**
- `src/logger_service.cpp`, `src/logger_core.cpp`, `src/logger_segment.cpp`, `src/logger_lifecycle.cpp` — Bench logger internals are more modular now.
- `src/write_orchestrator.cpp:101-438` and `src/main.cpp:304,392` — Control still has mixed reporting paths.
- `scripts/core/entrypoint.py` and `scripts/README.md` — helper orchestration is still script-led.

**Implications:**
- Priority 1 should still start in Control, but it can now borrow a clearer module shape from Bench.
- Priority 2 should be treated as a deliberate helper-surface reduction project, not assumed to happen automatically as more native commands land.

---

## Cross-Cutting Analysis

### Constraints
- Control is already direct-only, so the remaining logger work is consistency/refactor work rather than bridge removal.
- Bench logger improvements do not automatically reduce script surface because the repo still uses Python/PowerShell as workflow wrappers.
- Bench should remain the proof seam; shipping runtime ownership still belongs in Control.

### Risks
| Risk | Likelihood | Impact | Notes |
|------|-----------|--------|-------|
| Overestimating Control progress because native log files exist | M | H | The hard part left is unifying diagnostics and helper ownership, not just emitting CSV/JSONL. |
| Treating Bench modularization as equivalent to script reduction | H | M | The native logger improved, but orchestration is still wrapper-heavy. |
| Pulling too much Bench runtime behavior into Control | M | H | Bench remains a characterization/proof repo, not the shipping service/runtime. |

### Open Questions
- All questions answered.

---

## Recommendation

The current state is better than earlier for Bench logger architecture, but not materially better for Control logger cleanliness or Bench script reduction. Treat priority 1 as ready for implementation in Control now, using the current Bench logger decomposition as the structural reference. Treat priority 2 as a separate cleanup campaign after that, starting with script dispatch / audit / test wrappers rather than ingestion or deep analysis tooling.

