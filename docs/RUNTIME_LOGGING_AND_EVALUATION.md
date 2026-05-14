# Runtime Logging and Data Evaluation

## Status

Current as of 2026-05-14.

The packaged controller is now good enough for measured tuning of the current
profile: channels `0,1,2,3,4,5`, channel `6` blocked by live policy,
`control_loop.poll_tick_ms=50`, `write_cooldown_ms=50`, and
`deadband_pct=0.35`.

Implementation sequencing for the logging work lives in
`docs\LOGGING_IMPROVEMENT_PLAN.md`.

Recent local evidence:

- `release\runtime\logs\archive\svg_mb_control_control-loop_20260514_033423.csv`
  contained 19,432 rows. CPU/Tctl max was `86.625 C`, achieved loop interval
  averaged `50.641 ms`, radiator channel setpoints reached `71.40%`,
  `68.00%`, and `65.86%`, and the live status showed no overrun with process
  CPU around `0.29%`.
- `release\runtime\logs\archive\svg_mb_control_control-loop_20260514_035931.csv`
  contained 17,773 rows from a lower-heat/idle recovery run. CPU/Tctl max was
  `77.500 C`, achieved loop interval averaged `50.610 ms`, max interval was
  `58.150 ms`, average loop work was `4.314 ms`, average process CPU was
  `0.207%`, and no overrun rows were recorded.

Raw captures are local evidence. Do not commit full runtime CSV captures by
default; commit small summaries and decision records instead.

## Current Logging Surfaces

Control owns the runtime logging plane:

- `current_state.json` is the current telemetry snapshot.
- `control_runtime.json` is the current mode/status publication. For
  `control-loop`, schema version `3` includes timing fields, process CPU and
  memory fields, active log paths, and per-channel demand/setpoint state.
- `pending_writes.json` is the restore/recovery sidecar for active writes.
- `logs\svg_mb_control_output.csv` is the fixed-path live CSV mirror of the
  active chunk.
- `logs\archive\svg_mb_control_<mode>_<timestamp>.csv` is the retained CSV
  history.
- `logs\svg_mb_control_events.jsonl` is the append-only event stream for
  starts, rotations, writes, restores, policy refusals, sensor failures,
  circuit-breaker transitions, and sidecar warnings.

`control_runtime.json` is intentionally a status view, not the per-tick data
source. Use the CSV for timing and response analysis.

## What Is Working

- The logging plane is product-owned inside `SVG-MB-Control`; it does not need a
  sibling process or bridge helper.
- Control logs per-tick telemetry, fan state, setpoints, thermal-pressure boost,
  timing quality, and process resource cost.
- The 50 ms loop is currently observable and stable enough for the shipped
  channel set under local testing.
- JSONL events separate discrete control actions and failures from the dense CSV
  stream.
- Rotation and retention are local config fields, so long runs do not require
  external cleanup tooling.

This is a solid early-phase data substrate. The next improvement should be
better experiment accounting and automated summarization, not a wholesale
logging replacement.

## Gaps

- There is no run manifest that records workload label, ambient notes, build
  info, git commit, config hash, operator notes, or start/stop markers.
- There is no repo-owned analyzer that turns CSV plus events into a repeatable
  summary. Current analysis is still manual.
- CSV chunk files have no closed/ready marker. A reader must treat the active
  archive path as mutable while Control is running.
- The CSV has loop timing and process cost, but not per-sensor-group read
  durations. That limits deeper cadence diagnosis.
- Event rows have event type and detail, but no normalized severity or error
  code taxonomy yet.
- Status publication is rate-limited in the current implementation, so tools
  must not assume `control_runtime.json` updates every tick.
- Sensor-failure and circuit-breaker state is event-log visible but not exposed
  as explicit per-channel status fields yet. Circuit-breaker reset also needs a
  deliberate operator/runtime path if it should recover without process restart.
- There is no standard decision record that links a config change to before/after
  metrics.

## Evaluation Workflow

Use this loop for controller changes:

1. State the hypothesis before changing config. Example: "raising channel 4
   thermal-pressure max boost by 3% will reduce combined-load CPU/Tctl p90
   without increasing idle writes."
2. Capture a run label and notes: workload, start/stop times, ambient if known,
   subjective noise notes, active config path, build, and git commit.
3. Run one fixed profile at a time:
   - idle hold,
   - GPU step,
   - combined CPU plus GPU,
   - CPU-only diagnostic only when needed,
   - cooldown.
4. Collect the active CSV archive, `control_runtime.json`,
   `current_state.json`, and `svg_mb_control_events.jsonl`.
5. Summarize the run before tuning. Compute at least:
   - CPU/Tctl p50, p90, p99, max,
   - GPU core and memory p50, p90, p99, max,
   - achieved interval p50, p95, max, and overrun count,
   - loop work duration p50, p95, max,
   - process CPU and memory ranges,
   - writes per minute by channel,
   - setpoint p50, p90, max by channel,
   - max and saturation time for thermal-pressure boost,
   - downward setpoint steps above the configured deadband,
   - authority reassertions, policy refusals, write failures, sensor failures,
     and circuit-breaker events,
   - recovery time back to near-idle setpoints after load stops.
6. Change one class of knob at a time:
   - curve breakpoints,
   - thermal-pressure boost,
   - fall/release behavior,
   - deadband/cooldown,
   - channel membership.
7. Write a short decision record with before/after metrics and the exact config
   fields changed.

## Tuning Guidance

Do not jump to PID as the next controller model. The current controller is
already a practical nonlinear control stack:

- gain-scheduled feed-forward curves,
- CPU overlay on top of GPU envelope,
- asymmetric first-order demand smoothing,
- bounded slow integral trim through `thermal_pressure_*`,
- final slew limiting through rise/fall rates,
- deadband and cooldown quantization before writes.

Tune that model from data first:

- If high-load CPU/Tctl remains too high, raise the relevant CPU overlay point
  or the radiator thermal-pressure max boost. Prefer channels `4` and `5`
  before channel `1` unless the run shows otherwise.
- If GPU memory is high but CPU is acceptable, tune the GPU envelope curves for
  front-intake and case-airflow lanes before raising CPU overlay.
- If fans release too slowly after load, raise
  `thermal_pressure_fall_pct_per_sec` in small steps.
- If downward hot-zone steps are audible, lower the channel deadband or reduce
  fall alpha on that channel before changing rise behavior.
- Keep off-floor rise behavior intact unless a measured run proves it is too
  abrupt or too noisy.

## Next Actions

1. Add a small Control-owned analyzer that accepts a control-loop CSV and event
   JSONL path, then emits a compact Markdown or JSON summary with the metrics
   above.
2. Add a run manifest beside each capture or summary. Minimum fields:
   `run_id`, `profile`, `started_at`, `ended_at`, `config_path`,
   `config_sha256`, `build_info_path`, `git_commit`, `notes`, and artifact
   paths.
3. Add config/build identity to the CSV prologue so a standalone CSV can be
   traced back to the binary and config that produced it.
4. Add per-sensor-group timing fields only if cadence diagnosis needs deeper
   evidence than current loop work duration provides.
5. Add normalized event severity and error codes before building a dashboard or
   long-running ingestion service.
6. Expose per-channel `sensor_failed`, `consecutive_sensor_failures`,
   `circuit_breaker_open`, and `consecutive_write_failures` in
   `control_runtime.json` if those states are meant to drive operations.
7. Reuse ideas from the GPU programs where they fit: run manifests, segment
   accounting, deadline/cadence summaries, and compact decision records. Do not
   import tray UI, service orchestration, or external metrics infrastructure
   until the Control schema stabilizes.
