# FEAT-0002: CPU settings evidence logger

**Project:** svg-mb-control
**Status:** Implemented (load layer; `cpu_settings_label` / REQ-CPUSETTINGS-06 deferred)   **Version:** 0.2   **Updated:** 2026-06-04
**Namespace:** `REQ-CPUSETTINGS-*`
**Companion to:** `AGENTS.md`, `docs/RUNTIME_HOME.md`,
`docs/RUNTIME_LOGGING_AND_EVALUATION.md`, `docs/READ_LOOP.md`,
`docs/CONTROL_LOOP.md`
**Purpose:** capture enough first-party raw CPU activity evidence to evaluate
CPU-setting changes against temperature, without adding third-party tool
dependencies or presenting pre-digested conclusions.

## 1. Summary

This feature adds a first-party CPU-settings evidence surface to the runtime
logger. The goal is not to tune fans and not to score benchmarks. The goal is to
make non-benchmark CPU behavior derivable from raw data: at comparable
low/near-idle, background, or interactive activity, an operator can later compare
CPU temperature, CPU busy time, GPU context, and fan context before and after CPU
setting changes.

## 2. Problem & motivation  *(promotion gate 1)*

Current runtime logs can show CPU thermal result (`cpu_tctl_c`, `cpu_max_c`),
GPU context, fan duty/RPM, controller process CPU cost, and control response
sources. They do not log whole-machine CPU activity. `process_cpu_pct` is only
the `svg-mb-control` process cost, computed from process CPU time deltas in
`src/control/control_scheduler.cpp`; it is not system CPU load.

This means low/near-idle analysis currently has to infer CPU work from
temperature and GPU-cool filters. That is backwards for CPU-setting evaluation:
temperature is the result to compare, not the sole load classifier. The missing
minimum is first-party whole-system CPU activity data that can be joined to the
existing thermal and fan context.

## 3. Goals & non-goals

**Goals**
- Add the minimum first-party raw fields needed to derive whole-system CPU busy
  time per sample/window.
- Preserve existing CPU temperature, GPU context, fan context, and process CPU
  fields as the surrounding evidence.
- Keep the logged data raw enough that analyzer logic can derive idle,
  near-idle, background-work, bursty, and thermal-residual windows later.
- Allow an optional operator CPU-settings label so before/after windows can be
  grouped without guessing from timestamps alone.

**Non-goals**
- No fan-control behavior change.
- No benchmark scoring or workload result tracking in this feature.
- No third-party tool dependency, subprocess adapter, HWiNFO integration, or
  sibling-repo bridge.
- No pre-digested "good/bad" conclusion in the logger. Summaries and
  classifications are analyzer/reporting work built from the raw fields.
- No CPU package power, effective clocks, voltage, PPT/TDC/EDC, or per-core
  counters until a first-party source is designed and reviewed.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | Data collection uses in-repo code and OS APIs only; no external sensor process or third-party tool is introduced. |
| No fan write / start / stop / breaker reset outside an explicit live task | `AGENTS.md` §Live Runtime Safety | This is read-only telemetry. It must not start, stop, restart, reset breakers, or write fan duty. |
| Runtime sidecar / status / manifest schema stays backward-compatible | `docs/RUNTIME_HOME.md` | New fields are additive and nullable/blank when unavailable. Existing archives remain valid. |
| Tuning/evidence workflow stays documented | `docs/RUNTIME_LOGGING_AND_EVALUATION.md` | Implementation must document how the new raw fields support later CPU-setting comparison. |
| Control-computation identity stays unchanged | `docs/CONTROL_PIPELINE_MATH.md` | This feature records evidence only; it does not change curves, blending, cadence, boost, or write gates. |

## 5. Behavior specification

Proposed behavior, not implemented yet:

- The runtime logger records whole-system CPU time deltas alongside existing
  temperature, GPU, fan, and controller-process fields.
- The minimum raw fields are:
  - `system_cpu_idle_delta_ms`
  - `system_cpu_kernel_delta_ms`
  - `system_cpu_user_delta_ms`
  - `system_cpu_processor_count`
  - optional `system_cpu_busy_pct`, derived from the same deltas for operator
    convenience but not required for downstream derivation.
- `system_cpu_busy_pct` is a whole-machine estimate, not
  `svg-mb-control` process cost. The existing `process_cpu_pct` field keeps its
  current meaning.
- A sample with unavailable system CPU data leaves the new fields blank/null and
  emits no false zero.
- An optional CPU-settings label is recorded as metadata, not as a measured
  sensor. The default is blank. It can come from config or an explicit operator
  marker decided later.
- Analyzer/reporting can later classify windows from raw data. The logger does
  not decide whether a sample is idle, near-idle, background work, or thermal
  residual.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-CPUSETTINGS-01 | The logger must preserve current CPU temperature, GPU context, fan context, and `process_cpu_pct` semantics. |
| REQ-CPUSETTINGS-02 | The logger must add first-party whole-system CPU time evidence sufficient to derive CPU busy percentage per sample/window. |
| REQ-CPUSETTINGS-03 | The new whole-system CPU fields must be additive and nullable/blank when unavailable; existing CSV archives and runtime-home files must remain valid. |
| REQ-CPUSETTINGS-04 | The logger must not depend on third-party sensor tools, subprocess adapters, or sibling repos. |
| REQ-CPUSETTINGS-05 | The logger must not classify or present conclusions; idle/near-idle/background/thermal-residual labels are derived by analyzer/reporting later. |
| REQ-CPUSETTINGS-06 | The feature must provide a way to associate samples with an operator CPU-settings label without making that label a measured sensor value. |

## 7. Data / schema deltas

- New runtime CSV/status fields, additive:
  - `system_cpu_idle_delta_ms`
  - `system_cpu_kernel_delta_ms`
  - `system_cpu_user_delta_ms`
  - `system_cpu_processor_count`
  - `system_cpu_busy_pct` (optional derived convenience field)
  - `cpu_settings_label` (optional metadata)
- Config impact: undecided. A small optional config value or runtime marker may
  own `cpu_settings_label`; this needs a design decision before implementation.
- Schema/version impact: update `docs/RUNTIME_HOME.md` and
  `docs/RUNTIME_LOGGING_AND_EVALUATION.md` at implementation. Existing archives
  must parse with missing fields.

## 8. CLI / config / operator surface deltas

Minimum implementation can avoid new CLI by accepting a static optional label
from config. A later operator marker command may be useful if CPU settings
change while the runtime stays up.

Open operator-surface question: whether `cpu_settings_label` belongs in
`control.json`, a runtime-home marker/event file, or both. The current lean is:
static config label first, marker/event later only if needed.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/cpu-settings-evidence-logger-decision-2026-06-04.md`](../cpu-settings-evidence-logger-decision-2026-06-04.md) | Exact first-party source for whole-system CPU time, whether to log raw deltas only or raw deltas plus derived busy percentage, and where `cpu_settings_label` lives. | Current (2026-06-04): `GetSystemTimes`; raw deltas plus derived `system_cpu_busy_pct`; `cpu_settings_label` deferred. |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-CPUSETTINGS-01 | T, R | CSV/header compatibility tests and review against `docs/RUNTIME_HOME.md` |
| REQ-CPUSETTINGS-02 | T | unit/smoke test for system CPU delta calculation from sampled counters |
| REQ-CPUSETTINGS-03 | T, R | analyzer ingest tests with old archives missing the new fields |
| REQ-CPUSETTINGS-04 | R | code review: no third-party tool, subprocess, or sibling-repo dependency |
| REQ-CPUSETTINGS-05 | R | review logger code and docs: no baked-in activity classification |
| REQ-CPUSETTINGS-06 | T, R | test/config review for optional label propagation |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / tests).
- **B** = build/release gate (`.\build-release.ps1` /
  `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV/status/event evidence; read
  only unless explicitly authorized).
- **R** = code review against the cited contract doc.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Which first-party API owns whole-system CPU time sampling? | implementation | Windows system CPU time counters near existing scheduler/resource sampling |
| Should CSV log raw deltas only, or raw deltas plus `system_cpu_busy_pct`? | implementation | Raw deltas plus optional derived percent for convenience |
| Where does `cpu_settings_label` live? | implementation | Optional static config label; runtime marker later if needed |
| Should CPU package power be attempted now? | implementation | No; leave as missing until a first-party design exists |

## 12. Measurement gate & dependencies

- **Measurement gate:** does not change fan channels, cadence, write timing, or
  control math. It is read-only evidence logging.
- **Depends on:** existing runtime CSV/status/logging surfaces.
- **Build/test impact:** add CSV/header and analyzer-ingest compatibility tests
  when implemented. No `CONTROL_PIPELINE_MATH.md` update unless implementation
  changes control computation, which this feature must not do.

## 13. Promotion-gate checklist

- [x] 1. Problem stated as a named code/contract gap: current logs lack
  whole-system CPU activity and `process_cpu_pct` is process-local (§2).
- [x] 2. Stressed invariants identified — Repo Boundary, Live Runtime Safety,
  runtime-home schema, evidence workflow, control identity (§4).
- [x] 3. Required design decision record written and marked current (§9):
  `docs/cpu-settings-evidence-logger-decision-2026-06-04.md`.
- [x] 4. Concrete `REQ-CPUSETTINGS-*` IDs assigned (§6).
- [x] 5. Verification mapped to `Test-LocalCI` / review (§10).
- [x] 6. Confirmed it does not violate Live Runtime Safety or Repo Boundary and
  does not move the Measurement Gate baseline.
- [x] 7. Doctrine check: current behavior claims are grounded; proposed behavior
  is labeled as proposed; missing power/clock fields are named as non-goals for
  the minimum version.

> Gate 3 is settled (decision dated 2026-06-04). The whole-system CPU **load**
> layer (REQ-CPUSETTINGS-01..05) is implemented and verified (§14). The operator
> `cpu_settings_label` (REQ-CPUSETTINGS-06) is deferred — it carries its own
> open question (config key vs runtime marker, §8) and is the shared label with
> FEAT-0006. The **work/energy** layer (work-per-Joule, package power) is out of
> scope here and is specified separately in the `FEAT-0006`
> (cpu-work-energy-efficiency-evidence) spec.

## 14. Verification log  *(fill in after the feature is built)*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-CPUSETTINGS-01 | pass | `process_cpu_*`, GPU, fan, and temperature columns unchanged; only additive columns added in `BuildControlLoopCsvHeader`/`Row`. `Test-LocalCI.ps1` 7/7 ctest + 114 pytest pass. | 2026-06-04 |
| REQ-CPUSETTINGS-02 | pass | `system_cpu_idle/kernel/user_delta_ms`, `system_cpu_processor_count`, `system_cpu_busy_pct` from `GetSystemTimes` (`control_scheduler.cpp`). Derivation unit-tested in `core_smoke_tests.cpp::TestSystemCpuBusyDerivation` (66.667% from synthetic counters; not divided by processor count). | 2026-06-04 |
| REQ-CPUSETTINGS-03 | pass | Columns additive; consumers bind by header name (`analyze_csv.cpp` `GetField`, `dashboard.js` header map). `tests/test_analyze_ingest.py` ingests a control-loop CSV with none of the new columns and passes. | 2026-06-04 |
| REQ-CPUSETTINGS-04 | pass | No third-party tool / subprocess / sibling repo; uses Win32 `GetSystemTimes` only, beside the existing `GetProcessTimes` sample. | 2026-06-04 |
| REQ-CPUSETTINGS-05 | pass | Logger records raw deltas + derived busy percent only; no idle/near-idle/background/thermal-residual classification in the logger. | 2026-06-04 |
| REQ-CPUSETTINGS-06 | deferred | `cpu_settings_label` not implemented this change (decision §"Where `cpu_settings_label` lives"); remains an open item (§11) and the shared label with FEAT-0006. | 2026-06-04 |

**Spec vs. implementation deltas:** the `cpu_settings_label` (REQ-CPUSETTINGS-06)
was deferred per the 2026-06-04 decision; the spec's load layer
(REQ-CPUSETTINGS-01..05) shipped as written. The new fields land on the
**control-loop CSV** surface only; status-JSON (`control_runtime.json`) parity for
`system_cpu_busy_pct` is intentionally not added this change (the status surface
uses a 0-on-unavailable convention that conflicts with the CSV's blank-on-
unavailable "no false zero" rule). Status-JSON parity is a candidate follow-up.
