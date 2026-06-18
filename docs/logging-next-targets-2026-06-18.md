# Logging Next Targets

**Project:** svg-mb-control
**Status:** Proposed planning note; not implementation authorization
**Updated:** 2026-06-18
**Decision ids:** D-PWRLOG-1 follow-through, D-GPUCTX-1 proposal
**Companion docs:** `docs/as-is-logging-opportunities-2026-06-18.md`,
`docs/power-logging-flip-plan-2026-06-18.md`,
`docs/feat-0020-critical-evaluation-2026-06-18.md`,
`docs/feat-0020-power-logging-implementation-plan-2026-06-18.md`,
`docs/features/FEAT-0020-standard-control-loop-power-logging.md`,
`docs/features/FEAT-0021-standard-control-loop-gpu-workload-context-logging.md`

## 1. Recommended order

| Rank | Target | Doc owner | Why this order |
|---|---|---|---|
| 1 | Standard CPU/GPU power in the control-loop CSV | `FEAT-0020` | This directly answers the current comparison need: CPU package energy and GPU power visible in the same deployed control-loop rows. |
| 2 | Minimal GPU workload context beside GPU power | `FEAT-0021` | Once GPU power exists in standard rows, utilization, clocks, pstate, and VRAM explain why power changed without requiring foreground `evidence-log` alignment. |
| 3 | Analyzer run-level summaries from sidecars/events/manifests | Future spec if implemented | This improves reports without expanding the per-tick CSV. It can summarize health, supervisor, pending-write, manifest, and event counts per run. |
| 4 | SIO voltage/temperature label validation | `evidence-log`; FEAT-0007 if RAM/DIMM temps are promoted | The read path exists, but standard-log promotion needs label validity and a specific analysis question. |
| 5 | Sparse GPU diagnostics in standard logs | Future investigation only | Throttle, PCIe, voltage, GPU fans, rails, and raw thermal slots are useful for diagnosis but too wide for the default standard row. |
| 6 | CPU cycle/effective-frequency context | Existing FEAT-0006 gates | Already has standard CSV slots. Enable only for selected captures that need APERF/MPERF context. |

## 2. Immediate target: FEAT-0020

The next implementation target should stay narrow:

- enable existing FEAT-0006 CPU package-energy logging for the standard power
  logging profile;
- add the smallest GPU power slice to the standard control-loop CSV;
- keep CPU and GPU power observational only;
- add analyzer support for old/new archives; and
- document the operator flip/revert path around the safety-revert task and
  worker restart requirement.

Open gates before build:

- The critical review in `docs/feat-0020-critical-evaluation-2026-06-18.md`
  has been reconciled into FEAT-0020 v0.2 and D-PWRLOG-1.
- D-PWRLOG-1 in `docs/power-logging-flip-plan-2026-06-18.md` is Current.
- The GPU power cadence/field-set decision is closed for v1: per-tick read,
  cadence-agnostic five-field schema.
- Runtime evidence must still show the added GPU power logging path keeps the
  shipped 250 ms control-loop profile inside the measurement envelope; that
  evidence is deferred to a separately authorized live flip.

## 3. Follow-up target: FEAT-0021

Prepare, but do not build yet: `FEAT-0021` captures the follow-up GPU workload
context target. Its v1 scope should stay limited to fields that explain GPU
power:

- utilization;
- pstate;
- graphics and memory clocks;
- VRAM used/total;
- sample identity, timestamp, sample age, and acquisition marker.

It should reuse the same bounded in-repo GPU evidence sample path chosen for
FEAT-0020. It should not add throttle reasons, PCIe, voltage, GPU fans, rails,
or thermal-slot dumps to the default standard CSV.

Open gates before build:

- decide whether FEAT-0021 remains separate or is folded into a later FEAT-0020
  revision;
- settle exact field names and whether the context is enabled whenever standard
  GPU power logging is enabled;
- prove the combined GPU power/context sample cadence does not move the
  measurement baseline; and
- add analyzer compatibility tests for archives with and without the new fields.

## 4. Hold for later

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

## 5. Not next

Do not spend the next implementation slot on these unless a specific failure or
comparison run demands it:

- wall/PSU power;
- CPU PPT/TDC/EDC, SMU limit telemetry, voltage, or per-core frequency;
- per-core/per-CCD energy;
- HWiNFO or sibling-repo bridges;
- power-derived fan-control response sources or write gates.

Those are not simple log flips in the current repo.
