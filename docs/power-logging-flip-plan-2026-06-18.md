# Standard control-loop power logging flip plan

**Project:** svg-mb-control
**Status:** Current
**Updated:** 2026-06-18
**Decision id:** D-PWRLOG-1
**Companion spec:** `docs/features/FEAT-0020-standard-control-loop-power-logging.md`

## 1. Gates Found

This plan kept the shipped control law unchanged and opened only the logging path
needed to compare CPU package power against GPU power in the same standard
control-loop CSV. The implementation and live flip are now complete; current
evidence lives in `docs/feat-0020-live-flip-validation-results-2026-06-18.md` and
`docs/power-temp-comparison-snapshot-2026-06-18.md`.

### CPU package power gate

- Existing owner: `FEAT-0006` (`REQ-CPUEFF-*`).
- Existing control-loop CSV columns:
  `cpu_power_sample_id`, `cpu_power_window_ms`,
  `cpu_pkg_energy_delta_uj`, and `cpu_pkg_energy_acquisition`.
- Enable gate: `SVG_MB_CONTROL_RAPL_ENERGY_MODE` must be exactly `enabled`
  when `AmdReader` starts. Anything else is treated as disabled.
- Pre-flip live state observed 2026-06-18: process/user environment had
  `SVG_MB_CONTROL_RAPL_ENERGY_MODE=disabled`; the recent control-loop rows
  therefore carried `cpu_pkg_energy_acquisition=disabled` and no energy deltas.
- Restart gate: the reader consumes the environment at startup, so an operational
  flip requires an explicit worker restart through the documented repo workflow.
- Safety-revert gate: `scripts/Reset-EnergyToDisabled.ps1` and the
  `SVG-MB Energy Safety Revert` task drove the persistent environment back to
  disabled before the flip. The standard logging profile now disables that task
  while enabled, and `scripts/Set-EnergyLoggingProfile.ps1 -Disable` restores it.
- Trust marker gate: when enabled, the existing FEAT-0006 marker may remain
  `quarantine`; the logging flip must not promote it to `validated`. Validation
  stays the FEAT-0006 maintainer decision.

### CPU cycle gate

- Existing owner: `FEAT-0006` (`REQ-CPUEFF-01`).
- Enable gate: `SVG_MB_CONTROL_CPU_CYCLES_MODE=enabled`.
- Not required for watts-only comparison. Keep cycles disabled by default unless
  the comparison specifically needs effective-frequency context.

### GPU power gate

- Existing source: `GpuReader::SampleEvidence(...)` and the foreground
  `evidence-log` CSV path already expose `gpu_evidence_nvml_power_mw` and
  `gpu_evidence_power_source`.
- Pre-implementation gap: the standard `control-loop` CSV did not carry GPU
  power columns, only GPU temperature fields (`gpu_core_c`, `gpu_memjn_c`,
  `gpu_hotspot_c`).
- Schema gate: closed by FEAT-0020 with additive fields, analyzer schema v11,
  traceability rows, CSV/analyzer tests, and runtime docs updates.
- Cadence gate: closed by the 2026-06-18 live flip. The per-tick board-power read
  kept loop timing inside the shipped 250 ms profile in the recorded evidence.

## 2. Current decision

1. Keep control decisions temperature-based exactly as shipped.
2. Enable existing CPU RAPL package-energy logging for standard runtime only
   through an explicit operator/deployment profile:
   `SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled`.
3. Leave CPU cycles off unless a comparison run explicitly needs
   effective-frequency context.
4. Add additive GPU power fields to the standard control-loop CSV:
   `gpu_power_sample_id`, `gpu_power_time_ms`, `gpu_power_mw`,
   `gpu_power_source`, and `gpu_power_acquisition`.
5. Read GPU board power per control tick by adding a single in-repo NVML
   board-power read to the existing GPU thermal sample. The five-field schema is
   cadence-agnostic so a later cached cadence can reuse the same columns.
6. Keep GPU power unavailable as blank/null with an acquisition marker; never
   emit a false zero.
7. Update analyzer ingest/reporting with a v11 schema migration. GPU power is
   instantaneous board milliwatts, so reports summarize sample statistics
   (mean/p50/p90/max), not CPU-style energy/window math.
8. Use a reversible operator profile that disables/re-enables the
   `SVG-MB Energy Safety Revert` task when the standard power-logging profile is
   selected. This is an explicit governance decision: FEAT-0020 intentionally
   changes the earlier boot/logon guarantee that energy logging is always forced
   back off.
9. Record runtime evidence after the flip showing:
   CPU energy windows are present, GPU power windows are present, control-loop
   timing remains within the shipped cadence envelope, and no control response
   source names change because of power.

## 3. Completed implementation plan

1. Implemented against accepted `FEAT-0020`.
2. Added a narrow GPU power sample surface for control-loop logging.
   - Current decision: per-tick board-power read with a cadence-agnostic
     five-field schema.
   - Gate: proved the added read cost in the post-implementation live evidence
     window before standardizing the live flip.
3. Extended the control-loop CSV schema additively.
4. Extended analyzer ingest/reporting additively.
5. Added tests:
   - CSV header/row tests for new GPU power fields.
   - Analyzer old-archive compatibility tests.
   - Control identity review/tests showing power fields are not consumed by
     setpoint computation.
   - Config/operator workflow tests or script tests for the CPU RAPL logging
     profile and revert behavior.
6. Updated docs:
   - `README.md`
   - `docs/RUNTIME_HOME.md`
   - `docs/RUNTIME_LOGGING_AND_EVALUATION.md`
   - `docs/features/FEAT-0020-standard-control-loop-power-logging.md`
   - `docs/TRACEABILITY.md`
7. Validated locally with `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`.
8. After explicit live-runtime authorization, deployed/restarted through the
   documented repo workflow and collected the verification window.

## 4. Flip verification checklist

- Current worker command and config path captured.
- `SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled` visible to the worker environment at
  startup.
- `SVG_MB_CONTROL_CPU_CYCLES_MODE` stays disabled unless requested.
- First ten minutes of standard control-loop CSV contain nonempty CPU package
  energy sample ids and deltas.
- Standard control-loop CSV contains nonempty GPU power samples with
  `gpu_power_source` and `gpu_power_acquisition`.
- `control_health.json` remains healthy.
- Event tail contains no new write failures, no authority churn caused by the
  power fields, and no power-derived control response source.
- Analyzer report can summarize the run and older archives still ingest.

## 5. Non-goals

- No fan duty, curve, cadence, channel, breaker, restore, or control-source
  behavior change.
- No promotion of FEAT-0006 `quarantine` markers to `validated`.
- No sibling repo, HWiNFO, subprocess bridge, or third-party sensor dependency.
- No GPU power use in control decisions.
