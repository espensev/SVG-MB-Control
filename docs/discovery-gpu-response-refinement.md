# Discovery - GPU Response Refinement

Date: 2026-05-16
Status: Complete

## Goal

Evaluate the next largest refinement and simplification target, with GPU response treated as a first-class control outcome because it is central to overall thermal behavior.

## Questions

1. Where does GPU telemetry currently enter controller response and runtime logs?
2. What existing tooling can already measure GPU response quality?
3. What gaps remain before GPU response can be evaluated as a first-class control outcome?
4. Which areas are now most complex or duplicated after the logger/evidence split?
5. What is the next biggest refinement/simplification target by value, risk, and scope?

## Findings

### 1. GPU is already in the controller response path

The controller uses a GPU control envelope rather than only GPU core temperature. `src/channel_evaluator.cpp` defines `GpuControlEnvelopeC()` as the maximum of GPU core, memory junction, and hotspot when hotspot is valid. `src/control_loop.cpp` feeds that envelope into channel evaluation when runtime GPU data is available.

The packaged release config is already GPU-forward. Several chassis channels use `temp_blend: "gpu_only"` with CPU override curves layered on top, so GPU heat drives the normal case response while CPU temperature can still raise duty when needed.

Implication: the next GPU work should not start by changing the control math. The higher-value gap is that run evaluation does not yet measure GPU response with the same intent that the controller uses to command fans.

### 2. Logging has richer GPU data than evaluation uses

The normal runtime CSV includes narrow GPU fields:

- `gpu_available`
- `gpu_name`
- `gpu_last_warning`
- `gpu_core_c`
- `gpu_memjn_c`
- `gpu_hotspot_c`

The new evidence log can now collect a wider GPU set outside the control hot path, including clocks, utilization, VRAM, power, power source, rails, fans, PCIe state, voltage, throttle reasons, and raw thermal slots.

Implication: the data foundation is now better than the analysis surface. The analyzer still mostly sees the old narrow GPU view.

### 3. Evaluation does not yet model GPU response as the main outcome

`scripts/analyze_control_run.py` summarizes GPU core, memory junction, and hotspot, and also summarizes channels, setpoints, writes, thermal pressure, and down steps. The C++ analyzer CSV and DB ingest paths also preserve the same narrow GPU fields.

Missing pieces:

- Derived `gpu_envelope_c = max(core, memjn, hotspot-if-valid)` in analyzer summaries.
- GPU envelope p50, p90, p99, and max.
- GPU response windows for load onset, stabilization, and cooldown.
- Correlation between GPU envelope, channel setpoints, actual writes, and thermal pressure boost.
- Optional pairing with evidence-log GPU power, utilization, clocks, fan RPM, and throttle reasons.

Calibration artifacts already store `gpu_envelope_c_mean` for plant model capture, but that does not replace general run evaluation.

### 4. The next simplification pressure is logging schema duplication

After the logger and evidence split, `src/runtime_logging.cpp` has several manual header and row builders. That is workable, but it is now the main source of schema alignment risk because every new field must be added in matching header and row code.

The analyzer has a similar duplicated shape across:

- CSV parsing structs.
- SQLite table columns.
- Ingest bind order.
- Python analysis script field names.
- Tests and fixtures.

Implication: widening GPU evaluation will touch this duplication. It is the right moment to make a narrow schema helper only where it removes row/header mismatch risk, rather than doing a broad logging rewrite first.

### 5. Ranking of next targets

1. GPU response evaluation and analyzer widening.
   Highest value. It directly answers whether the controller is doing the right thing for the most important heat source. Risk is moderate because it touches analyzer schema and fixtures, but it can be staged.

2. Runtime logging schema helper.
   High simplification value. It reduces future mistakes in CSV header and row alignment, especially for evidence fields. Best done after the exact GPU metrics are chosen.

3. Control-loop tick/write extraction.
   Still valuable because `control_loop.cpp` remains broad, but part of the pure policy work has already moved into `channel_evaluator.cpp`. Further extraction is higher risk and should follow stronger GPU run metrics.

4. Timestamp utility cleanup.
   Low risk and tidy, but not large enough to be the next big target.

5. Per-sensor timing/cadence diagnostics.
   Useful if run evaluation shows unexplained jitter or stale data, but not the immediate bottleneck.

## Recommended Next Step

Make GPU response evaluation the next work item.

Stage it this way:

1. Add derived `gpu_envelope_c` to the analyzer path for control-loop CSVs, matching the controller's envelope rule.
2. Extend the Python analyzer summary with GPU envelope percentiles, max, and simple response-window metrics.
3. Add setpoint/write correlation around GPU envelope peaks so a run can answer whether fans responded early enough and strongly enough.
4. Decide whether to ingest evidence-log GPU power/utilization/fan data into the C++ analyzer DB now or keep it as a second-stage pairing by timestamp.
5. After the metrics are in place, simplify the runtime logging/analyzer schema builders where the GPU widening created obvious duplication.

This keeps the controller hot path stable, preserves the richer evidence sampling outside that path, and turns the new GPU evidence into actionable evaluation rather than just wider logs.

