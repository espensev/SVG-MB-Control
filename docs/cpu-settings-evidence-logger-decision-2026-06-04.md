# CPU Settings Evidence Logger Decision - 2026-06-04

Status: proposed design decision for
`docs/features/FEAT-0002-cpu-settings-evidence-logger.md`. Settles FEAT-0002
promotion gate 3. Not normative until accepted by the maintainer.

**Companion to:** `docs/features/FEAT-0002-cpu-settings-evidence-logger.md`,
`docs/RUNTIME_HOME.md`, `docs/RUNTIME_LOGGING_AND_EVALUATION.md`, and the
separate `FEAT-0006` (cpu-work-energy-efficiency-evidence) spec.

## Problem

Runtime control-loop logs record the controller *process* CPU cost
(`process_cpu_pct`, `process_cpu_delta_ms`), computed from `GetProcessTimes`
deltas in `SampleProcessResources` / `UpdateTimingResources`
(`src/control/control_scheduler.cpp:52-115`). They record no **whole-system** CPU
activity, so low/near-idle analysis has to infer system CPU work from temperature
and GPU-cool filters — backwards for evaluating CPU-setting changes, where
temperature is the result to compare, not the load classifier.

FEAT-0002 adds the minimum first-party whole-system CPU activity evidence. This
decision settles three open questions from FEAT-0002 §11: the first-party source,
whether to log raw deltas only or raw deltas plus a derived busy percent, and
where the operator `cpu_settings_label` lives.

This decision is scoped to the **time** layer only. The **work/energy** layer
(instructions/cycles per Joule, package power) is FEAT-0006 and is out of scope
here; busy time is its time-normalization context, not a substitute for it.

## Options considered

### Source of whole-system CPU time

**Option A — `GetSystemTimes` (chosen).** The Win32 `GetSystemTimes(&idle,
&kernel, &user)` call returns idle, kernel, and user time as `FILETIME` 100 ns
counters, already summed across all logical processors, with `kernel` including
`idle`. It is a first-party OS API, needs no driver, and sits next to the
existing `GetProcessTimes` sampling in `SampleProcessResources`.

- No third-party tool, subprocess, or sibling-repo dependency (REQ-CPUSETTINGS-04).
- Same delta technique already used for the process counters; reuses the existing
  ~1 s resource window in `tick_runner.cpp`.

**Option B — `NtQuerySystemInformation(SystemProcessorPerformanceInformation)`.**
Per-processor idle/kernel/user via the native API.

- Rejected for v1: gives per-core resolution this feature does not need (Tier 3,
  FEAT-0006 §11), and depends on a semi-documented native API. `GetSystemTimes`
  provides the whole-machine aggregate the spec asks for with a documented call.

**Option C — PDH `\Processor Information(_Total)\% Processor Time`.** Performance
Data Helper counter.

- Rejected for v1: heavier query setup, and it returns a pre-derived percentage
  rather than the raw counters the spec wants kept raw (REQ-CPUSETTINGS-05).

### Raw deltas only, or raw deltas plus derived busy percent

**Chosen: raw deltas plus an optional derived `system_cpu_busy_pct`.** Log the
raw idle/kernel/user deltas (so the analyzer can re-derive anything) *and* a
convenience busy percent computed from the same deltas in the same tick. The
percent is redundant with the raw fields by design; it is for operator
convenience, and the analyzer is not required to trust it.

### Where `cpu_settings_label` lives

**Chosen: deferred.** `cpu_settings_label` is not implemented in this change. It
carries its own undecided question (config key vs runtime marker, FEAT-0002 §8)
and is not needed to capture system CPU load. It stays an open item in FEAT-0002
§11 and is the natural shared label with FEAT-0006 §11.

## Decision

Adopt **`GetSystemTimes`**, logging **raw idle/kernel/user deltas plus a derived
`system_cpu_busy_pct` and the processor count**, with **`cpu_settings_label`
deferred**.

New control-loop fields on `RuntimeControlLoopTimingState`
(`src/runtime/runtime_csv_rows.h`), all additive and nullable:

- `system_cpu_idle_delta_ms`
- `system_cpu_kernel_delta_ms` (kernel includes idle)
- `system_cpu_user_delta_ms`
- `system_cpu_processor_count`
- `system_cpu_busy_pct` (derived convenience field)

## Derivation (normative for implementation)

Over a resource window from sample `p` to sample `c`, with each counter in 100 ns
units summed across all logical processors:

```
idle_delta   = c.idle   - p.idle
kernel_delta = c.kernel - p.kernel      # includes idle_delta
user_delta   = c.user   - p.user
total_delta  = kernel_delta + user_delta
busy_delta   = total_delta - idle_delta # = (kernel_delta - idle_delta) + user_delta
system_cpu_busy_pct = 100 * busy_delta / total_delta
```

- The `*_delta_ms` fields are `delta_100ns / 10000.0`. Because the counters are
  summed across all logical processors, an `*_delta_ms` value can exceed the
  wall-clock window (roughly `processor_count × window_ms`); this is expected and
  documented, and is why `system_cpu_processor_count` is logged alongside.
- **`system_cpu_busy_pct` must not be divided by `system_cpu_processor_count`.**
  `GetSystemTimes` counters are already whole-machine aggregates and `busy_pct`
  is the ratio of two such aggregates, so it is already core-normalized to
  `[0, 100]`. This differs from `process_cpu_pct`, which *is* divided by the
  processor count because `GetProcessTimes` returns single-process time that must
  be normalized to whole-machine capacity (`control_scheduler.cpp:111-114`).
- Guard exactly as the process path does: require a previous valid sample, a
  positive window, and monotonic counters (`c >= p` on each); on a counter going
  backwards or a non-positive `total_delta`, leave all five fields blank/null and
  emit no false zero (REQ-CPUSETTINGS-03, and FEAT-0002 §5 "no false zero").
- `system_cpu_processor_count` is the existing `ActiveProcessorCount()`
  (`control_scheduler.cpp:47-50`), recorded only on a window that produced a valid
  delta; blank otherwise.

## Apply order (normative for implementation)

1. **Sample.** In `SampleProcessResources` add a `GetSystemTimes` read into new
   `ProcessResourceSample` fields (`valid_system_cpu`, `system_idle_100ns`,
   `system_kernel_100ns`, `system_user_100ns`), next to the existing
   `GetProcessTimes` read.
2. **Derive.** In `UpdateTimingResources` compute the five fields per the
   derivation above, into `RuntimeControlLoopTimingState`.
3. **Carry.** In `tick_runner.cpp`, on the same ~1 s resource window that updates
   `last_process_cpu_*`, store `last_system_cpu_*`, then set them on the tick's
   `RuntimeControlLoopTimingState` (mirrors the existing process-cost carry,
   `tick_runner.cpp:295-333`).
4. **Emit.** Add the five columns to `BuildControlLoopCsvHeader` /
   `BuildControlLoopCsvRow` immediately after `process_private_bytes`, grouping
   the system CPU block with the process-cost block.

## Consequences

- New columns are additive; CSV consumers bind by header name
  (`src/analyze/analyze_csv.cpp` `column_index`/`GetField`;
  `tools/eval_dashboard/dashboard.js` header map), so existing archives without
  the columns still parse (REQ-CPUSETTINGS-03).
- `process_cpu_pct` keeps its current meaning (controller-process cost);
  `system_cpu_busy_pct` is the new whole-machine estimate (REQ-CPUSETTINGS-01/02).
- No control-computation identity change; `docs/CONTROL_PIPELINE_MATH.md` is
  unaffected. Read-only evidence; no fan write, start/stop, or breaker reset
  (`AGENTS.md` §Live Runtime Safety).
- The logger records no idle/near-idle/background/thermal-residual classification
  and no `cpu_settings_label`; both remain analyzer/operator work
  (REQ-CPUSETTINGS-05/06, the latter deferred).
- `docs/RUNTIME_HOME.md` and `docs/RUNTIME_LOGGING_AND_EVALUATION.md` gain the new
  columns at implementation (`AGENTS.md` §Change Checklist).

## Verification

- `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`: a C++ test derives
  `system_cpu_busy_pct` and the `*_delta_ms` fields from two synthetic counter
  samples (including the kernel-includes-idle relation and the no-divide-by-count
  rule), and confirms `BuildControlLoopCsvHeader`/`Row` stay aligned and contain
  the five named columns.
- Backward-compatibility: analyzer/ingest parse of an archive missing the columns
  still succeeds (name-bound `GetField`).
- Code review vs this decision and `docs/RUNTIME_HOME.md`: source is
  `GetSystemTimes`, busy percent is not divided by processor count, fields blank
  when unavailable, no `cpu_settings_label` introduced.
