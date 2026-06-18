# As-Is Logging Opportunities

**Date:** 2026-06-18
**Status:** notes only; not implementation authorization
**Scope:** signals the repo already samples or already writes somewhere today.

This note inventories what else can be logged "as is" for comparison work. "As
is" means no new hardware backend, sibling process, HWiNFO bridge, fan-control
input, or write-policy change. Mirroring any new field into the standard
control-loop CSV is still a runtime-schema change and must be covered by an
accepted feature spec before product-code work starts.

Companion context:

- `docs/power-logging-flip-plan-2026-06-18.md` - current CPU/GPU power logging
  flip plan.
- `docs/logging-next-targets-2026-06-18.md` - recommended target sequence
  after this inventory.
- `docs/features/FEAT-0020-standard-control-loop-power-logging.md` - draft spec
  for standard control-loop CPU package power and GPU power.
- `docs/features/FEAT-0021-standard-control-loop-gpu-workload-context-logging.md`
  - draft follow-up spec for GPU workload context beside GPU power.
- `docs/discovery-next-logging-targets.md` - older completed evidence-log
  discovery note; useful background, but historical.

Source files checked:

- `src/runtime/runtime_csv_rows.cpp` - standard read/control/evidence CSV
  headers and rows.
- `src/runtime/runtime_csv_rows.h` - control-loop timing/channel state and
  evidence-log state structs.
- `src/runtime/gpu_evidence_csv.cpp` - GPU evidence CSV field list.
- `src/hardware/gpu_reader.h` - GPU thermal/evidence sample contracts.
- `README.md` and `docs/RUNTIME_LOGGING_AND_EVALUATION.md` - operator-facing
  logging/analyzer contracts.

## Current inventory

### Already in standard control-loop CSV

These fields are already time-aligned with control decisions:

- CPU/GPU thermal context: `cpu_tctl_c`, `cpu_max_c`, `gpu_available`,
  `gpu_core_c`, `gpu_memjn_c`, `gpu_hotspot_c`, `gpu_name`, and
  `gpu_last_warning`.
- Fan state per channel: label, RPM, raw tach, tach validity, duty raw,
  duty percent, mode raw, manual override, policy block, and effective write
  allowed.
- Control intent per channel: observed temperature, setpoint, feedforward,
  boost-stage contribution, low-band boost/debt/signal, primary temp source,
  response source, write reason, total writes, write active, and baseline
  captured.
- Cadence and cost: tick count, loop start/finish, work duration, intended and
  achieved interval, slip, overrun, process CPU, process memory, system CPU
  busy context, and cadence transient.
- CPU package-energy/cycle slots from FEAT-0006: `cpu_power_sample_id`,
  `cpu_power_window_ms`, `cpu_pkg_energy_delta_uj`,
  `cpu_pkg_energy_acquisition`, `cpu_cycles_sample_id`,
  `cpu_cycles_window_ms`, `cpu_aperf_delta`, `cpu_mperf_delta`, and
  `cpu_cycles_acquisition`. These slots are useful only when their environment
  gates are enabled.

### Already in foreground `evidence-log`

These signals are already emitted by `--mode evidence-log`, outside the
controller hot path:

- Backend read cost and freshness: evidence poll interval, sample duration,
  per-backend read durations, and changed flags for runtime snapshot, AMD, GPU
  thermal, fan state, fan tach, SIO voltage, SIO temperature, and GPU evidence.
- SIO evidence: fan tach high/low raw bytes, 16 voltage slots with raw and
  label, and 23 SIO temperature slots with raw, half raw, validity, and label.
- GPU evidence: sample mode, requested mode, detail, GPU identity, timestamp,
  delta time, temperatures, clocks, pstate, utilization, VRAM, NVML power,
  power source, core voltage, PCIe throughput/link state, throttle reasons, GPU
  fan readings, power rails, and raw thermal slots.

## Best candidates to mirror into standard control logs

### P0 - Power comparison fields

This is the current FEAT-0020 lane.

- CPU package power: do not add a derived watts field. Keep using FEAT-0006
  `cpu_pkg_energy_delta_uj` over `cpu_power_window_ms`, keyed by
  `cpu_power_sample_id`; analyzer/reporting derives watts over distinct sample
  ids.
- GPU power: mirror the smallest useful slice from GPU evidence:
  `gpu_power_sample_id`, `gpu_power_time_ms`, `gpu_power_mw`,
  `gpu_power_source`, and `gpu_power_acquisition`.
- Required guard: blank numeric values when unavailable; never emit false zero.
- Required proof: show loop timing/process cost stays inside the measurement
  envelope before making the flip standard.

### P1 - GPU workload context for interpreting power

These are high-value comparison companions because they explain why GPU power
and GPU thermal load differ between runs:

- GPU utilization: `gpu_evidence_util_gpu_pct`,
  `gpu_evidence_nvml_util_gpu_pct`, and `gpu_evidence_nvml_util_mem_pct`.
- GPU clocks/pstate: `gpu_evidence_pstate`,
  `gpu_evidence_clock_graphics_mhz`, `gpu_evidence_clock_memory_mhz`,
  `gpu_evidence_nvml_clock_graphics_mhz`, and
  `gpu_evidence_nvml_clock_memory_mhz`.
- VRAM pressure: `gpu_evidence_vram_used_mb` and
  `gpu_evidence_vram_total_mb`.

Recommendation: keep FEAT-0020 v1 focused on power unless these are explicitly
accepted into the same spec. If added, emit them from the same bounded GPU
evidence sample as GPU power, with sample id/time/age so repeated cached values
are obvious.

### P1 - Read-cost and sample-age proof

If any GPU evidence is mirrored into the control-loop CSV, the log should also
carry enough timing context to prove it is not hurting cadence:

- `gpu_power_read_ms` or `gpu_evidence_read_ms` for the mirrored sample path.
- `gpu_power_sample_age_ms` if the implementation uses a cached evidence
  sample cadence.
- acquisition marker strings for disabled, unavailable, cached, and live
  states.

Recommendation: log only the cost/age fields needed to defend the measurement
gate. Do not mirror every evidence-log backend timing field into standard
control logs unless a control-loop problem specifically needs it.

### P2 - GPU diagnostic context

These are useful for diagnosing odd power/temperature behavior, but they are not
needed in every standard control row:

- `gpu_evidence_throttle_reasons`
- `gpu_evidence_voltage_core_mv`
- `gpu_evidence_pcie_tx_kb_s`
- `gpu_evidence_pcie_rx_kb_s`
- `gpu_evidence_pcie_link_gen`
- `gpu_evidence_pcie_link_width`
- GPU fan level/RPM fields
- GPU power-rail fields

Recommendation: keep these in `evidence-log` by default. Mirror them only for a
specific investigation or if the analyzer starts needing them to explain
standard comparison runs.

### P2 - SIO voltage and temperature evidence

The repo already has SIO voltage and SIO temperature capture in `evidence-log`.
This is useful for board/rail/environment correlation, but it is wider and less
settled than the standard control-loop CSV needs today.

Good current use:

- Run `evidence-log` when validating sensor labels, rail behavior, or possible
  DIMM/board temperature sources.
- Use analyzer/notes to summarize stable labels and ranges instead of copying
  all SIO slots into every control tick.

Do not promote into standard control-loop logs until the label validity,
sampling cadence, and use case are explicit. DIMM/RAM temperature telemetry is
already parked under FEAT-0007 and is not buildable by this note.

### P2 - Fan tach raw high/low bytes

The standard control-loop CSV already has the combined tach raw value and RPM.
`evidence-log` adds raw high/low bytes.

Recommendation: keep high/low bytes in `evidence-log` unless diagnosing tach
validity, byte-order, or SIO parsing. They are not a strong default standard-log
candidate.

### P3 - CPU cycle context for selected runs

The APERF/MPERF fields are already present in standard control-loop CSV but are
off by default. They are useful when comparing effective-frequency behavior to
power and temperature, but they should not be part of the default power-logging
flip unless the run explicitly needs that context.

Recommendation: leave `SVG_MB_CONTROL_CPU_CYCLES_MODE=disabled` in the standard
power profile. Enable it only for named comparison captures, and keep effective
frequency derived by analyzer/reporting.

### P3 - Sidecar and event summaries

Some useful data is already present, but it belongs as run-level summary rather
than per-row CSV expansion:

- `control_health.json`: health status, producer freshness, active process.
- `control_runtime.json`: active mode, PID, timing, active logs, and status
  publication.
- `control_supervisor.json`: supervisor/worker state.
- `pending_writes.json`: active restore/recovery sidecar state.
- JSONL events: starts, rotations, writes, restores, policy refusals,
  sensor failures, circuit-breaker transitions, and normalized severity/error
  codes.
- Manifests: producer identity, config/runtime-policy hashes, row/event counts,
  archive path, and active mirror path.

Recommendation: teach analysis/reporting to attach compact run-level summaries
when needed. Avoid repeating sidecar state on every tick unless there is a
specific per-tick question.

## Not available "as is"

These would require a new acquisition path, a new contract decision, or external
dependencies and should not be treated as simple log flips:

- Wall/PSU power.
- CPU PPT/TDC/EDC or SMU limit telemetry.
- CPU voltage and per-core frequency as direct runtime fields.
- Per-core or per-CCD energy.
- C-state/P-state residency.
- DIMM/RAM temperatures in the standard control path.
- HWiNFO or sibling-repo sensor bridges.
- Any power-derived fan-control source, setpoint term, boost term, write gate,
  or safety override.

## Recommended next cut

1. Finish FEAT-0020 narrowly: enable existing CPU package-energy logging for the
   standard profile, add GPU power fields, and keep control math unchanged.
2. If the comparison runs need more GPU context, amend FEAT-0020 or open a
   follow-up spec for GPU utilization/clocks/pstate/VRAM from the same bounded
   GPU evidence sample.
3. Keep SIO voltage/temperature and fan high/low tach evidence in
   `evidence-log` until a named analysis task proves they need to be
   time-aligned with standard control-loop rows.
4. Add only the minimum read-cost/sample-age fields needed to defend cadence
   when mirroring evidence into the control log.
5. Keep derived quantities in analyzer/reporting where possible. Runtime logs
   should carry raw sample identity, raw units, timestamps/windows, and
   acquisition markers so downstream math stays auditable.
