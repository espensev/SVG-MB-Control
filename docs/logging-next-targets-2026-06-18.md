# Logging Next Targets

**Project:** svg-mb-control
**Status:** Current logging-target decision record; FEAT-0020/0021/0022 completed or verified as noted
**Updated:** 2026-06-20
**Decision ids:** D-PWRLOG-1 follow-through, D-GPUCTX-1 current, D-LOGHEALTH-1 current
**Companion docs:** `docs/as-is-logging-opportunities-2026-06-18.md`,
`docs/power-logging-flip-plan-2026-06-18.md`,
`docs/feat-0020-critical-evaluation-2026-06-18.md`,
`docs/feat-0020-power-logging-implementation-plan-2026-06-18.md`,
`docs/features/FEAT-0020-standard-control-loop-power-logging.md`,
`docs/features/FEAT-0021-standard-control-loop-gpu-workload-context-logging.md`,
`docs/features/FEAT-0022-runtime-logging-failure-visibility.md`,
`docs/runtime-logging-health-decision-2026-06-20.md`

## 1. Recommended order

| Rank | Target | Doc owner | Why this order |
|---|---|---|---|
| 1 | Standard CPU/GPU power in the control-loop CSV | `FEAT-0020` | **Done 2026-06-18.** CPU package energy and GPU power are visible in the same deployed control-loop rows and preserved in the power/temp comparison snapshot. |
| 2 | Runtime logging failure visibility | `FEAT-0022` | **Done 2026-06-20.** Required CSV/status/snapshot/event-log failure visibility and analyzer consistency diagnostics are implemented. |
| 3 | Minimal GPU workload context beside GPU power | `FEAT-0021` | **Done 2026-06-20 (T/R; live M pending).** Cached context rows now explain why GPU power changed without requiring foreground `evidence-log` alignment. |
| 4 | Analyzer run-level summaries from sidecars/events/manifests | Future spec if implemented | This improves reports without expanding the per-tick CSV. It can summarize health, supervisor, pending-write, manifest, and event counts per run. |
| 5 | SIO voltage/temperature label validation | `evidence-log`; FEAT-0007 if RAM/DIMM temps are promoted | The read path exists, but standard-log promotion needs label validity and a specific analysis question. |
| 6 | Sparse GPU diagnostics in standard logs | Future investigation only | Throttle, PCIe, voltage, GPU fans, rails, and raw thermal slots are useful for diagnosis but too wide for the default standard row. |
| 7 | CPU cycle/effective-frequency context | Existing FEAT-0006 gates | Already has standard CSV slots. Enable only for selected captures that need APERF/MPERF context. |

## 2. Completed target: FEAT-0020

FEAT-0020 stayed narrow and is now implemented:

- enabled existing FEAT-0006 CPU package-energy logging for the standard power
  logging profile;
- added the smallest GPU power slice to the standard control-loop CSV;
- kept CPU and GPU power observational only;
- added analyzer support for old/new archives; and
- documented the operator flip/revert path around the safety-revert task and
  worker restart requirement.

Closed gates:

- The critical review in `docs/feat-0020-critical-evaluation-2026-06-18.md`
  has been reconciled into FEAT-0020 v0.2 and D-PWRLOG-1.
- D-PWRLOG-1 in `docs/power-logging-flip-plan-2026-06-18.md` is Current.
- The GPU power cadence/field-set decision is closed for v1: per-tick read,
  cadence-agnostic five-field schema.
- Runtime evidence shows the added GPU power logging path keeps the shipped
  250 ms control-loop profile inside the measurement envelope; the live flip was
  executed under explicit authorization and recorded in
  `docs/feat-0020-live-flip-validation-results-2026-06-18.md`.
- The current comparison anchor is
  `docs/power-temp-comparison-snapshot-2026-06-18.md`, which preserves CPU and
  GPU watts beside temperatures from the standard control-loop CSV.

## 3. Immediate evidence-integrity target: FEAT-0022

`FEAT-0022` captures the current logging-health surface. Slices A/B plus status
retry and Slice C now cover CSV/archive/mirror/manifest write visibility,
event-log append fallback, status/snapshot publish retry correctness, and
analyzer consistency diagnostics. It should stay closed unless new evidence
shows another evidence-sink failure class.

Implemented slices shipped 2026-06-20:

- `WriteRow(...)` failures are observed by control-loop, read-loop, and
  evidence-log;
- `RuntimeCsvLogger` records sink/detail for archive, mirror, flush, and
  manifest failures;
- `runtime_logging.csv_write_failed` and
  `runtime_logging.csv_write_recovered` are rate-limited by sticky in-memory
  failure state;
- event-log append failure writes sticky `logging_health.json` and health/status
  JSON degrades while that marker is active;
- status/snapshot publish failures emit sticky
  `runtime_logging.status_publish_*` and
  `runtime_logging.snapshot_publish_*` events, failed control status publishes
  keep forced retry active, and failed control snapshot publishes do not advance
  retry timing;
- `analyze report` flags manifest-declared/archive-ingested/latest-mirror CSV
  row-count disagreement as `running_csv_manifest_consistency_warning` for
  running sessions and
  `closed_csv_manifest_consistency_suspect_evidence` for closed runs; and
- `runtime_csv_archive_tests`, `runtime_event_log_tests`,
  `runtime_status_tests`, `test_control_loop.py`, `test_read_loop.py`,
  `test_runtime_health.py`, `analyze_report_tests`, `test_analyze_ingest.py`,
  plus full `Test-LocalCI` verify the slices.

Optional follow-up, not required for FEAT-0022 closure:

- keep CSV byte-cap retention separate unless evidence ties it directly to
  evidence loss;
- consider a future manifest revision only if operators need to distinguish
  logical rows from proven persisted rows in the manifest itself.

## 4. Implemented follow-up target: FEAT-0021

`FEAT-0021` captures the follow-up GPU workload context target and is now
implemented as D-GPUCTX-1. Its v1 scope stays limited to fields that explain GPU
power:

- utilization;
- pstate;
- graphics and memory clocks;
- VRAM used/total;
- sample identity, timestamp, sample age, and acquisition marker.

The implementation keeps FEAT-0020's per-tick `ThermalFast` + NVML board-power
read unchanged, then refreshes a cached in-repo GPU context sample at most once
per 1000 ms through the GPU reader fast/rare sample family. It does not add
throttle reasons, PCIe, voltage, GPU fans, rails, or thermal-slot dumps to the
default standard CSV.

Closed decisions:

- FEAT-0021 remains separate from FEAT-0020.
- The field set is the 11-field slice:
  `gpu_context_sample_id`, `gpu_context_time_ms`,
  `gpu_context_sample_age_ms`, `gpu_context_acquisition`,
  `gpu_util_gpu_pct`, `gpu_util_mem_pct`, `gpu_pstate`,
  `gpu_clock_graphics_mhz`, `gpu_clock_memory_mhz`, `gpu_vram_used_mb`, and
  `gpu_vram_total_mb`.
- There is no config knob in v1; the fields are always present in the
  control-loop CSV and nullable when context is unavailable.
- Analyzer schema v12 ingests the optional fields and `analyze report` emits a
  `gpu_context` summary only from present data.

Remaining deployment check:

- `REQ-GPUCTX-04` is T/R verified by the bounded cached design and
  `Test-LocalCI`, but live 250 ms runtime M evidence was not collected in this
  implementation change. On first live deployment, compare achieved interval,
  slip/overrun, process CPU%, and health against the current measurement
  envelope.

## 5. Hold for later

Run-level analyzer summaries are a better next target than widening every row
with sidecar state. Candidate summary sources already exist:

- `control_health.json`
- `control_runtime.json`
- `control_supervisor.json`
- `pending_writes.json`
- runtime manifests
- JSONL event counts and severity/error-code counts

This likely deserves its own feature spec only when implementation approaches,
because it changes analyzer/report output but not control behavior.

SIO voltage/temperature and raw tach high/low bytes should stay in
`evidence-log` until a named validation or hardware-label task needs them.
DIMM/RAM temperature promotion remains parked under FEAT-0007.

## 6. Not next

Do not spend the next implementation slot on these unless a specific failure or
comparison run demands it:

- wall/PSU power;
- CPU PPT/TDC/EDC, SMU limit telemetry, voltage, or per-core frequency;
- per-core/per-CCD energy;
- HWiNFO or sibling-repo bridges;
- power-derived fan-control response sources or write gates.

Those are not simple log flips in the current repo.
