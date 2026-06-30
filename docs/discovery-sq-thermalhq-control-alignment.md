# SQ/ThermalHQ Alignment for Control Planning - 2026-06-26

## Scope

Read-only cross-repo discovery across:

- `D:\Development\Thermals\SQ-control`
- `D:\Development\Thermals\ThermalHQ`
- this repo, `D:\Development\Thermals\SVG-MB\SVG-MB-Control`

No sibling repo files, live runtime state, service state, scheduled tasks, fan
duty, or raw runtime CSV captures were changed.

## Questions

1. What role do `SQ-control` and `ThermalHQ` assign to `SVG-MB-Control`?
2. Which telemetry, status, health, power, load, and response surfaces does
   Control already provide that unblock further SQ planning?
3. Which SQ plans would be unsafe if they inferred beyond current evidence?
4. What should Control provide as docs/spec inputs before any new behavior work?
5. What must stay out of scope to preserve the standalone Control boundary?

## Findings

### 1. ThermalHQ is evidence, not an implementation source

ThermalHQ describes itself as a shared documentation workspace for comparing the
existing controllers and extracting guidance for SQ-Control
(`D:\Development\Thermals\ThermalHQ\README.md:3-4`). Its README explicitly says
to use ThermalHQ as evidence, not implementation source, and points the current
SQ queue back to `SQ-control` docs (`D:\Development\Thermals\ThermalHQ\README.md:21-29`).

The bridge note frames SVG-MB-Control as the slow multi-channel motherboard/SIO
position controller with crash-recovery journaling, health files, CSV/analyze
reports, and hardware-write safety, while NVG contributes fast GPU ARC control,
SHM/status, command-file IPC, fan sweep calibration, and tune-advisor feedback
(`D:\Development\Thermals\ThermalHQ\SQ_CONTROL_NEEDS_BRIDGE_2026-06-26.md:11-18`).

Implication: Control should not grow a sibling bridge or runtime adapter for
SQ. It should remain the reference implementation and evidence source for the
motherboard/chassis half.

### 2. SQ is sim-complete, not ready for live fan ownership

SQ-Control's README says the current state is M2/WS-10b sim telemetry/control,
with real hardware pending (`D:\Development\Thermals\SQ-control\README.md:6-22`).
The current-needs document repeats that M3/WS-11 real SIO/NVAPI/PawnIO, service
mode, HIL, cutover, analyzer DB, production dashboard history/config, and
release packaging are future work (`D:\Development\Thermals\SQ-control\docs\CURRENT_NEEDS_2026-06-26.md:13-20`).

SQ's next gate is Phase 0, not immediate chassis-real or GPU-real work. The
listed prerequisites include corrupt-sidecar quarantine, off-hot-path journal
persistence, GPU backend selection, both-domain restore/reconcile coverage,
dual-tick timing budget, telemetry/status/SHM ownership freeze, and GPU
write-policy enforcement (`D:\Development\Thermals\SQ-control\docs\CURRENT_NEEDS_2026-06-26.md:55-75`).

Implication: Control can help SQ proceed by documenting the mature evidence and
safety contracts SQ is trying to port, but Control should not take live
cutover, service, or SQ-domain ownership inside this repo.

### 3. Control already has the evidence shape SQ wants

Control is a standalone runtime repo that owns its executable, configs, runtime
state, vendored dependencies, and release artifacts, and does not depend on
sibling repos at runtime (`README.md:3-6`). Its repo boundary assigns process
lifetime, config, policy, runtime state, and recovery behavior to this repo, and
keeps bridge executables out of the runtime contract (`README.md:28-34`).

The highest-value Control surfaces for SQ planning are:

- `current_state.json`: current telemetry snapshot with fan `rpm`, `tach_raw`,
  `duty_raw`, `mode_raw`, `tach_valid`, `write_allowed`, `policy_blocked`, and
  `effective_write_allowed` (`docs\RUNTIME_HOME.md:57-84`).
- `control_runtime.json`: status-rate-limited control-loop view with process id,
  timing fields, process cost, log paths, hardware-access states, active profile
  identity, and controlled-channel state (`docs\RUNTIME_HOME.md:115-147`).
- Control-loop CSV: per-tick source of timing, fan response, process cost,
  whole-system CPU activity, CPU package-energy windows, CPU cycle windows, GPU
  board power, GPU workload context, profile identity, and per-channel demand /
  setpoint / response fields (`docs\RUNTIME_HOME.md:528-568`).
- Health/status: the `--health --json` path merges runtime, supervisor, logging
  health, stop-request, and pending-write state into states `healthy`,
  `degraded`, `stale`, `stopped`, and `failed`, with exit codes 0/1/2/3
  (`docs\RUNTIME_HOME.md:317-351`).
- Analyzer: `analyze ingest` and `analyze report` import runtime manifests, CSV
  archives, events, and plant-model captures, then report idle/load/cooldown
  temperatures, per-channel response, response delay, event counts, power
  summaries, and diagnostic flags (`README.md:305-413`).

Implication: Control does not need a new live data plane to support SQ
planning. A documented, version-aware field map and compact fixtures would be
enough for SQ to consume the Control reference safely.

### 4. SQ's main gap is diagnostic evidence ownership, not donor evidence

SQ's own contracts say `sq_engine_status.json` is a status sidecar and that
`current_state.json` is a contract target for a full `Sensors` diagnostic
snapshot, but the 2026-06-26 note says the source has only the path helper and
the writer still needs implementation or verification before diagnostics treat
it as live input (`D:\Development\Thermals\SQ-control\docs\CONTRACTS.md:559-610`).

SQ's health follow-up says the same: `current_state.json` is listed as an
evidence option, but the checkout does not implement a writer; options are to
extend status/SHM, add a dedicated diagnostic sidecar, implement the promised
writer, or use recent CSV rows first (`D:\Development\Thermals\SQ-control\docs\HEALTH_ANOMALY_FOLLOWUP_REVIEW_2026-06-26.md:12-46`).

The lowest-risk path for SQ health checks is recent CSV evidence because SQ
chassis CSV rows already include the fan tach and policy fields that status/SHM
lack. SQ's contract preserves SVG-MB chassis CSV column names and includes
`tach_raw`, `tach_valid`, `write_allowed`, `policy_blocked`, and
`effective_write_allowed` in the per-fan group
(`D:\Development\Thermals\SQ-control\docs\CONTRACTS.md:771-807`).

Implication: Control should document the exact current-state, CSV, and analyzer
field meanings as the donor reference. SQ should still own the implementation
choice for its own `current_state.json`, diagnostic sidecar, or CSV reader.

### 5. Health, E2, and anomaly need source separation

ThermalHQ warns not to infer health from thin live status alone; it points to
SVG-MB's richer runtime/current-state/CSV evidence and NVG's recent-window
models as reasons to choose diagnostic evidence before verdicts
(`D:\Development\Thermals\ThermalHQ\SQ_CONTROL_NEEDS_BRIDGE_2026-06-26.md:52-62`).

SQ's E2 review aligns with that split. Command POSTs need serialization before
they are specced (`D:\Development\Thermals\SQ-control\docs\LOCAL_API_EXPORTER_E2_REVIEW_2026-06-26.md:28-35`),
`/api/v1/status` must define SHM-vs-JSON merge semantics and null handling
(`D:\Development\Thermals\SQ-control\docs\LOCAL_API_EXPORTER_E2_REVIEW_2026-06-26.md:53-61`),
and `/healthz` should be exporter liveness while `/readyz` describes engine
data readiness (`D:\Development\Thermals\SQ-control\docs\LOCAL_API_EXPORTER_E2_REVIEW_2026-06-26.md:63-83`).
The review verdict keeps E2 as a separate non-elevated, loopback-only,
read-only sidecar first (`D:\Development\Thermals\SQ-control\docs\LOCAL_API_EXPORTER_E2_REVIEW_2026-06-26.md:158-164`).

Implication: Control should not add an in-engine HTTP/API server to help SQ.
If Control adds any export surface, it needs its own accepted feature spec and
must keep the exporter outside the timing-sensitive fan-control path.

### 6. Power and load evidence are useful, but trust levels matter

Control's runtime docs distinguish CPU package energy from GPU board power:
CPU package watts are derived later from distinct `cpu_power_sample_id` windows,
with acquisition states including `disabled`, `unavailable`, `quarantine`, and
`validated`; average package power is not directly logged
(`docs\RUNTIME_HOME.md:580-594`). GPU board power is an instantaneous NVML board
reading, logging-only, de-duplicated by `gpu_power_sample_id`, and not a
time-weighted energy integral (`docs\RUNTIME_HOME.md:596-608`). GPU workload
context is also logging-only and never a response source, write gate, breaker
input, or fan-duty input (`docs\RUNTIME_LOGGING_AND_EVALUATION.md:260-280`).

The local 2026-06-26 telemetry review confirms the practical state: GPU power is
well populated across current archives, while CPU package power is populated but
currently marked `quarantine`; CPU load can be characterized, but CPU
efficiency cannot be finalized because cycle coverage is sparse
(`docs\reviews\2026-06-26-fan-power-load-response-review.md:171-224`).

Implication: SQ/ThermalHQ planning can use Control GPU power and workload
context confidently as load evidence. CPU package power should be labeled by
acquisition state, and work-per-energy claims need longer cycle/all-core
coverage or explicit validation.

## What Control Should Provide Next

### A. Land a Control-owned external-consumer field map

Create a small docs artifact, or promote this discovery into one, that maps:

- `current_state.json` fan evidence to SQ diagnostic terms
- `control_runtime.json` health/timing/profile fields to SQ status terms
- control-loop CSV response fields to SQ chassis CSV/analyzer fields
- power/load fields and trust states to SQ load/diagnostic terms
- analyzer report blocks to SQ P3/P4 planning terms

This is docs-only if it describes existing fields. If it adds, renames, or
promotes any runtime field, CLI output, schema, status value, or new export
artifact, it must go through the feature-intake gate.

### B. Add compact fixtures, not raw captures

SQ would benefit from tiny sanitized fixtures showing:

- a `current_state.json` fan entry with tach/write-policy evidence
- a `control_runtime.json` controlled-channel entry with response/source fields
- a short CSV header/sample row with power/load/context fields
- a minimal analyzer report JSON snippet with response and power blocks

Do not commit full runtime CSV captures by default; Control docs already state
raw captures are local evidence and small summaries/decision records are the
right committed form (`docs\RUNTIME_LOGGING_AND_EVALUATION.md:42-43`).

### C. Keep power/load trust labels explicit

Any Control-to-SQ field map should say:

- GPU board power is instantaneous board watts, not an energy integral.
- CPU package power is derived from energy windows and must be de-duplicated by
  sample id.
- Acquisition states are part of the contract; `quarantine` is not `validated`.
- GPU workload context and CPU/GPU power are logging-only evidence, never fan
  control inputs.

### D. Use existing specs before new behavior

The current active Control spec queue already covers most behavior-relevant
work:

- FEAT-0005 for actuation confirmation evidence.
- FEAT-0017 / FEAT-0024 for response retunes, both held behind measurement.
- FEAT-0018 for adaptive cadence, held behind characterization.
- FEAT-0020 / FEAT-0021 for power and GPU workload logging.
- FEAT-0022 for logging-health visibility.
- FEAT-0023 for active profile identity and restart-selected profiles.

If the next step is an external export contract or cross-repo readiness package,
add a new feature spec only if it changes runtime/schema/CLI behavior. A
docs-only field map can proceed without a new feature spec.

## Recommended SQ-Planning Notes

1. Keep M3 Phase 0 first. Do not start real chassis/GPU lanes before journal,
   status/SHM ownership, write policy, restore/reconcile, and timing gates are
   frozen.
2. For one-click health, choose the diagnostic evidence source first. Recent
   chassis CSV is the lowest ABI risk; a verified `current_state.json` writer or
   dedicated diagnostic sidecar is better for instant UX.
3. Status/SHM alone must not publish fan-stall `ok`, live-control scope, or
   high-confidence hardware health unless it carries tach validity, write-policy
   evidence, backend/source mode, and freshness.
4. E2 should start as read-only transport: `/metrics`, `/api/v1/status`,
   `/healthz`, and likely `/readyz`. Command POSTs need mutex/exclusive-create
   delivery and ack semantics first.
5. Predictive anomaly should stay behind WS-12/13/14 analyzer/history work, not
   one-tick status rules.

## Out Of Scope

- No Control runtime dependency on `SQ-control`, `ThermalHQ`, NVG, or any sibling
  repo.
- No in-engine HTTP server or web parser in `svg-mb-control.exe`.
- No SQ service/HIL/cutover action from this repo.
- No live fan writes, scheduled-task changes, controller restarts, or policy
  flips for this discovery.
- No raw runtime CSV capture committed by default.

## Bottom Line

Control already gives SQ the reference evidence it needs: tach-valid fan state,
effective write policy, current health/status, per-tick response attribution,
power/load context, runtime manifests, events, and offline analyzer reports.
The missing piece is a small, explicit consumer contract and fixture set so SQ
can copy the semantics without depending on this repo at runtime. Any new
Control behavior or schema meant for that contract should be spec-gated first.
