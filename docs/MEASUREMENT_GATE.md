# Measurement Gate

## Purpose

Before more control tuning, cadence changes, or broader mixed-input controller
work proceeds in `SVG-MB-Control`, the runtime needs a focused characterization
pass. The current direct runtime is good enough to measure, but it is not yet
measured well enough to justify faster polling, faster writes, or more advanced
strategy work.

UI work is out of scope for this gate.

## Why This Is Blocking

- Current loop timing is fixed-delay-after-work, so configured cadence is not
  the same as achieved cadence.
- Current logging captures values and control state, but not the timing-quality
  fields needed to evaluate loop behavior properly.
- Existing defaults are intentionally conservative, and the older rate
  assumptions in the workspace should be treated as stale until remeasured on
  the current machine.

See `docs\discovery-polling-logging-state.md` for the detailed review and source
evidence behind this gate.

## Required Steps Before More Work Here

1. Re-characterize AMD cadence, passive AMD plus SIO cadence, and fan-write
   response on the current machine using Bench-grade tooling.
2. Add a GPU cadence probe path for the vendored GPU telemetry slice so mixed
   CPU/GPU control is not tuned blind.
3. Extend Control logging with timing-quality fields before raising rates:
   - loop start/end wall clock
   - loop duration
   - intended interval
   - achieved interval
   - overrun or slip indicator
   - optional per-sensor-group read duration
4. Keep full per-tick logging during characterization. Do not add steady-state
   suppression first.
5. Only after the above is complete, decide whether to change:
   - sensor poll cadence
   - control-loop cadence
   - write cadence or cooldown policy
   - status-publication cadence
   - optional steady-state log filtering

## What Should Wait

The following work should not move forward until the required steps above are
done:

- faster `poll_ms` or `control_loop.poll_tick_ms` beyond the packaged `50 ms`
  control tick
- more aggressive write cadence or cooldown tuning beyond the packaged `50 ms`
  write cooldown
- mixed-input control tuning based on CPU/GPU blend timing
- logging suppression meant to reduce per-tick output
- controller strategy work that assumes the current rates are already known-good

## Exit Criteria

This gate is cleared when:

- cadence data exists for AMD, SIO, and GPU inputs used by this repo
- fan-write response has been rechecked on the current machine
- Control logs can show achieved cadence and loop timing quality
- a measured recommendation exists for sensible poll and write behavior

The packaged live profile now uses `control_loop.poll_tick_ms=50` and
`write_cooldown_ms=50` to avoid multi-second fan-write staircases. The gate
remains in force for faster sensor polling, channels beyond `0,1,2,3,4,5`, and
any cadence below the current 50 ms control/write profile.

Until then, this repo should treat characterization and timing instrumentation
as the next required work, not optional cleanup.
