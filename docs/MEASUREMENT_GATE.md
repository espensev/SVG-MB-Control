# Measurement Gate

## Current Status

As of 2026-05-14, the original packaged `200 ms` control gate is superseded.
The current packaged live controller uses `control_loop.poll_tick_ms=50`,
`write_cooldown_ms=50`, `deadband_pct=0.35`, and live channels
`0,1,2,3,4,5` with channel `6` blocked by policy.

This does not mean all future cadence work is open-ended. It means the current
50 ms profile is the measured baseline, and future changes must be evaluated
against that baseline.

## Purpose

Before faster cadence, additional live channels, or broader mixed-input
controller strategy work proceeds in `SVG-MB-Control`, the runtime needs a
focused characterization pass. The current direct runtime is good enough to
measure and tune the shipped profile, but changes beyond that profile still need
explicit evidence.

UI work is out of scope for this gate.

## What This Blocks

- Faster than `50 ms` control ticks.
- Faster than `50 ms` write cooldown.
- Adding channel `6` or any other currently blocked live channel.
- Strategy changes that assume CPU/GPU/SIO cadence is already known at a higher
  rate.
- Steady-state log suppression that would remove the data needed for tuning.

See `docs\discovery-polling-logging-state.md` for the detailed review and
source evidence behind the original gate. Treat that file as historical where it
mentions fixed-delay scheduling or missing loop-timing fields; the newer runtime
has fixed-start-period loop timing fields and process-resource fields.

## Required Steps Before Expanding Beyond The Current Profile

1. Re-characterize AMD cadence, passive AMD plus SIO cadence, GPU telemetry
   cadence, and fan-write response on the current machine.
2. Keep full per-tick logging during characterization. Do not add steady-state
   suppression first.
3. Summarize each run with the workflow in
   `docs\RUNTIME_LOGGING_AND_EVALUATION.md`.
4. Only after evidence exists, decide whether to change:
   - sensor poll cadence,
   - control-loop cadence,
   - write cadence or cooldown policy,
   - status-publication cadence,
   - live channel membership,
   - optional steady-state log filtering.

## Current Evidence

- `docs\discovery-steady-response-control.md` covers the 2026-05-14 high-heat
  response pass. The important result is that the current 50 ms profile is
  usable for channels `0-5`: no live overrun, process CPU around `0.29%`, and
  radiator channels reaching meaningful setpoints during sustained heat.
- The later local run
  `release\runtime\logs\archive\svg_mb_control_control-loop_20260514_035931.csv`
  was lower heat and showed no overruns, `50.610 ms` average achieved interval,
  `58.150 ms` max achieved interval, and average process CPU around `0.207%`.

## Exit Criteria

For faster cadence, more live channels, or a different controller strategy, the
gate is cleared when:

- cadence data exists for AMD, SIO, and GPU inputs used by this repo,
- fan-write response has been rechecked on the current machine,
- Control logs can show achieved cadence and loop timing quality,
- a measured recommendation exists for sensible poll and write behavior,
- a compact summary records the data that justified the change.

For the current shipped profile, the gate is clear enough to continue measured
tuning. Every future change should leave behind a compact summary of the data
that justified it.
