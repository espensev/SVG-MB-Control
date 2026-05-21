# Discovery - GPU Response Refinement

Date: 2026-05-16
Status: Complete; superseded by the 2026-05-21 analyzer widening.

Current status, 2026-05-21: the recommended analyzer work has landed. The
Python analyzer derives `gpu_envelope_c`, reports GPU envelope percentiles and
peak/threshold timing, records channel setpoints around GPU peaks, and writes
compact decision records. The native analyzer ingest/report path also stores and
reports `gpu_envelope_c`. The remaining work is live-run evaluation and optional
schema-helper cleanup, not another controller math change.

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

Implication: the next GPU work should not start by changing the control math.
The higher-value gap has moved from analyzer implementation to run-backed
evaluation using the GPU-envelope metrics now available.

### 2. Logging has richer GPU data than evaluation uses

The normal runtime CSV includes narrow GPU fields:

- `gpu_available`
- `gpu_name`
- `gpu_last_warning`
- `gpu_core_c`
- `gpu_memjn_c`
- `gpu_hotspot_c`

The new evidence log can now collect a wider GPU set outside the control hot path, including clocks, utilization, VRAM, power, power source, rails, fans, PCIe state, voltage, throttle reasons, and raw thermal slots.

Implication: the controller hot path still does not need a math change. The
analysis surface now has enough GPU-envelope coverage for run-backed tuning, so
the next step is to use it on real GPU/combined-load runs.

### 3. Evaluation now models GPU response as a first-class outcome

`scripts/analyze_control_run.py` summarizes GPU core, memory junction, hotspot,
and derived GPU envelope, then relates GPU envelope peaks and threshold windows
to channel setpoints, writes, and thermal-pressure response. The C++ analyzer
CSV and DB ingest paths also preserve `gpu_envelope_c`.

Implemented pieces:

- Derived `gpu_envelope_c = max(core, memjn, hotspot-if-valid)` in analyzer summaries.
- GPU envelope p50, p90, p99, and max.
- GPU response windows for load onset, stabilization, and cooldown.
- Correlation between GPU envelope, channel setpoints, actual writes, and thermal pressure boost.

Remaining optional piece:

- Optional pairing with evidence-log GPU power, utilization, clocks, fan RPM, and throttle reasons.

Calibration artifacts also store `gpu_envelope_c_mean` for plant model capture.
That complements, rather than replaces, the general run evaluation path.

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

1. Live GPU/combined-load response evaluation using the widened analyzer.
   Highest value. It directly answers whether the controller is doing the right thing for the most important heat source. Risk is low because it should collect and summarize data before changing curves.

2. Runtime logging schema helper.
   High simplification value. It reduces future mistakes in CSV header and row alignment, especially for evidence fields. Best done narrowly where the GPU/evidence widening exposed duplication.

3. Control-loop tick/write extraction.
   Still valuable because `control_loop.cpp` remains broad, but part of the pure policy work has already moved into `channel_evaluator.cpp`. Further extraction is higher risk and should follow stronger GPU run metrics.

4. Timestamp utility cleanup.
   Low risk and tidy, but not large enough to be the next big target.

5. Per-sensor timing/cadence diagnostics.
   Useful if run evaluation shows unexplained jitter or stale data, but not the immediate bottleneck.

## Recommended Next Step

Run GPU response evaluation with the analyzer that now exists.

Stage it this way:

1. Capture fixed idle, GPU step, combined CPU plus GPU, and cooldown profiles.
2. Generate Markdown summary, manifest, and decision record for each run.
3. Compare GPU envelope peaks and threshold windows against channel setpoints,
   writes, and response boost.
4. Tune GPU curve breakpoints only after the summaries show a repeatable gap.
5. Decide whether evidence-log GPU power/utilization/fan data needs timestamp
   pairing or whether the control-loop envelope fields are enough.
6. Simplify runtime logging/analyzer schema builders only where this evaluation
   exposes row/header duplication risk.

This keeps the controller hot path stable, uses the richer evidence sampling
outside that path, and turns the new GPU evidence into actionable evaluation
rather than just wider logs.
