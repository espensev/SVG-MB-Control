# Control Loop

## Purpose

`control-loop` is the long-running direct control path. It samples temperatures
and fan state in-process, calculates channel setpoints from config curves, and
applies writes through `SVG-MB-SIO`.

## Inputs

Top-level config fields used by `control-loop`:

- `runtime_home_path`
- `runtime_policy_path`
- `log_rotate_hours`
- `log_retain_days`
- `control_loop`

`control_loop` must contain:

- `poll_tick_ms`
- `write_cooldown_ms`
- `deadband_pct`
- `control_hold_ms`
- `curve_shape`
- `rise_rate_pct_per_min`
- `fall_rate_pct_per_min`
- `cpu_temp_label`
- non-empty `channels`

Each channel defines:

- `channel`
- `temp_blend`
- `min_duty_pct`
- `curve`

Optional channel overrides:

- `write_cooldown_ms`
- `deadband_pct`
- `control_hold_ms`
- `curve_shape`: `linear` or `smootherstep`
- `rise_rate_pct_per_min`
- `fall_rate_pct_per_min`
- `cpu_override_curve`: optional CPU/Tctl curve evaluated separately from
  `temp_blend`; the loop commands the higher duty from `curve` and
  `cpu_override_curve`
- `demand_smoothing_rise_alpha`: optional pre-rate-limit setpoint EMA alpha
  for rising demand
- `demand_smoothing_fall_alpha`: optional pre-rate-limit setpoint EMA alpha
  for falling demand
- `decay_latch_above_pct`: optional demand threshold for bounded fall behavior
- `decay_latch_pct_per_min`: optional maximum decay rate for smoothed demand
- `thermal_pressure_start_c`: optional sustained-heat threshold for the
  slow thermal-pressure boost
- `thermal_pressure_full_c`: temperature where the boost accumulates at its
  full configured rise rate
- `thermal_pressure_rise_pct_per_sec`: boost accumulation rate while observed
  temperature remains above `thermal_pressure_start_c`
- `thermal_pressure_fall_pct_per_sec`: boost decay rate once observed
  temperature falls below `thermal_pressure_start_c`
- `thermal_pressure_max_boost_pct`: maximum extra duty added after demand
  smoothing and before the normal rate limiter
- `cpu_low_soak_start_c`: optional low/medium CPU sustained-heat threshold
- `cpu_low_soak_full_c`: CPU temperature where the low-soak term accumulates at
  its full configured rise rate
- `cpu_low_soak_release_c`: CPU temperature at or below which the low-soak term
  decays
- `cpu_low_soak_rise_pct_per_min`: low-soak accumulation rate while CPU remains
  in the configured band
- `cpu_low_soak_fall_pct_per_min`: low-soak decay rate
- `cpu_low_soak_max_boost_pct`: maximum extra duty from low/medium CPU soak

Config loading validates basic ranges and required curve/channel structure. A
bad control config should fail at startup instead of producing undefined fan
behavior.

`smootherstep` uses the standard quintic blend `t^3 * (6t^2 - 15t + 10)`,
evaluated in Horner form in the curve lookup path.

## Runtime Flow

1. Resolve config, runtime home, and runtime policy.
2. Initialize the direct fan backend.
3. On each tick, sample AMD, GPU, and fan telemetry in-process.
4. Append the sampled row to the active CSV chunk and mirror it to the fixed
   live CSV path.
5. Capture the baseline duty and mode for each configured channel.
6. Blend temperatures according to `temp_blend`.
7. Interpolate the configured curve and clamp with `min_duty_pct`.
8. If `cpu_override_curve` is present and CPU telemetry is available,
   interpolate it against CPU/Tctl and use the higher duty.
9. Smooth the raw demand and apply bounded decay, then add any configured
   thermal-pressure boost and CPU low-soak boost before the normal rate limiter.
   This allows
   intermediate PWM steps while preventing repeated high-temperature up/down
   writes caused by small sensor swings.
10. Detect repeated missing primary temperature input for a channel and enter a
    safe full-speed setpoint for that channel until the sensor recovers.
11. Skip writes blocked by deadband, cooldown, runtime policy, or an open
    per-channel circuit breaker.
12. Record a pending-write sidecar entry before each applied write.
13. Append durable JSONL events for loop start, rotations, baseline capture,
    writes, restores, sensor failures, sidecar warnings, circuit-breaker
    transitions, and failures.
14. Restore the captured baseline once the hold window expires or shutdown is
    requested through the console handler or `stop.request.json`. A
    `control_hold_ms` of `0` holds the control write until shutdown/restart
    instead of periodically restoring.

## Outputs

`control-loop` writes:

- `runtime\current_state.json`
- `runtime\control_runtime.json`
- `runtime\pending_writes.json` while a write is active
- `runtime\logs\svg_mb_control_output.csv`
- `runtime\logs\archive\svg_mb_control_control-loop_<timestamp>.csv`
- `runtime\logs\svg_mb_control_events.jsonl`

`control_runtime.json` includes loop-level counters, timing-quality fields, the
active worker `process_id`, active log paths, and per-channel totals plus last
observed values. The status JSON is rate-limited, so it is a live status view
rather than the per-tick data source. The control-loop CSV carries the same
timing fields per row:

- `loop_started_wall_clock`
- `loop_finished_wall_clock`
- `loop_work_duration_ms`
- `loop_intended_interval_ms`
- `loop_achieved_interval_ms`
- `loop_slip_ms`
- `loop_overrun`
- `process_cpu_delta_ms`
- `process_cpu_pct`
- `process_working_set_bytes`
- `process_private_bytes`

`loop_achieved_interval_ms` is measured between tick starts. The control loop
uses fixed-start-period scheduling, so normal rows should land near
`loop_intended_interval_ms`; overruns show up when work exceeds the requested
period.

The process CPU and memory fields are emitted in both `control_runtime.json` and
the control-loop CSV so fast polling/write profiles can be watched for resource
cost. CPU percent is a rolling roughly one-second process average; memory fields
are sampled on each loop row.

Each controlled channel also publishes `last_thermal_pressure_boost_pct` and
`last_cpu_low_soak_boost_pct` in `control_runtime.json`, with matching
`channelN_thermal_pressure_boost_pct` and `channelN_cpu_low_soak_boost_pct`
columns in the control-loop CSV. These values are slow leaky-integral terms
added on top of the base curve/EMA demand.

The control-loop CSV also includes `channelN_feedforward_pct` and
`channelN_correction_pct`. Feedforward is the raw curve/overlay demand before
the control loop applies smoothing, thermal pressure, and rate limits;
correction is the final setpoint minus that feedforward term.

Response attribution is emitted as `last_response_source` in
`control_runtime.json` and `channelN_response_source` in the CSV. Current
sources are `primary_curve`, `cpu_override`, `sensor_safe_mode`, and optional
modifiers such as `+thermal_pressure`. CSV rows also include
`channelN_write_reason`, which is `first_write`, `setpoint_delta`,
`authority_reassert`, `write_failed`, or `none` for ticks without a write.

The current status JSON also publishes `last_raw_demand_pct` and
`last_smoothed_demand_pct` so a live reader can distinguish the curve/overlay
demand from the smoothed demand and final rate-limited setpoint.

Per-channel failure state is also surfaced in `control_runtime.json`:
`sensor_failed`, `consecutive_sensor_failures`, `circuit_breaker_open`, and
`consecutive_write_failures`. The JSON remains rate-limited; use the event log
for exact transition timing.

## Lifecycle Commands

With the packaged `control.json`, a zero-argument launch or `--start` starts a
hidden supervisor and returns the shell prompt. The supervisor launches the
worker with `--run-foreground`, writes supervisor/worker stdout and stderr logs
in the runtime home, and restarts the worker after an unexpected non-zero exit.
Startup/config failures are reported immediately instead of entering a restart
loop.

The normal Windows install path is
`Install-SVG-MB-ControlScheduledTask.ps1`. It registers an at-logon scheduled
task named `SVG-MB Control` for the current user, runs it elevated, and starts
the controller immediately. The task action invokes `svg-mb-control.exe --start`
with the packaged `control.json`, so logon startup uses the same supervised
launch path as a manual operator start.

Operator commands use the same runtime-home resolution as the active config:

- `--status` reads `control_runtime.json` and checks `process_id`.
- `--status --json` or `--health --json` emits the machine-readable health
  contract with exit codes `0=healthy`, `1=degraded`, `2=stale/stopped`, and
  `3=failed`.
- `--stop` writes `stop.request.json`; the worker exits through the normal
  restore/shutdown path.
- `--restart` performs the same cooperative stop and only launches a new
  supervisor after the previous worker reports stopped.

`Install-SVG-MB-ControlWatchdogScheduledTask.ps1` adds a separate watchdog task
that checks health at logon and every minute. It restarts only `stale` or
`stopped` states; `degraded` is visible but not restarted, and `failed` is left
for operator review.

Passing `--mode control-loop --config <path>` keeps the loop attached to the
current terminal and does not add supervisor restart behavior.

## Policy Behavior

`runtime_policy_path` is resolved locally inside Control.

- `writes_enabled=false` blocks writes globally.
- `blocked_channels` blocks specific channels.
- The published fan payload exposes `write_allowed`, `policy_blocked`, and
  `effective_write_allowed`.
- The packaged live control policy controls Channels `0,1,2,3,4,5`.
- The packaged loop cadence is `poll_tick_ms=50` with `write_cooldown_ms=50`
  and a sub-one-percent deadband. The goal is to preserve authority and emit
  small intermediate PWM steps rather than audible multi-second staircases.
- While `control-loop` runs, Control requests a 1 ms Windows timer period so the
  50 ms fixed-start-period loop is not stretched by the default scheduler
  quantum.
- On this host, the radiator Noctua lanes inside that set are `1,4,5`. Excluding
  Channel `6` does not mean "no radiator control"; it only keeps the separate
  pump-like or still-ambiguous lane out of the shipped live loop.
- Lanes `2,3` are treated as slow front-intake airflow lanes and use
  higher floors plus rate-limited smootherstep response.
- The shipped curves use GPU envelope as the primary case-airflow signal and a
  mandatory CPU/Tctl overlay so Cinebench plus max CUDA load can raise channels
  even when the GPU curve alone would not.
- The radiator Noctua lanes `1,4,5` intentionally use different CPU override
  points and decay rates so they do not ramp as one synchronized block.
- The same radiator lanes also carry a slow thermal-pressure boost. Sustained
  high heat can add duty even when the instantaneous curve/EMA would otherwise
  hover too low or drop during small temperature dips.
- Lanes `1,4,5` also carry a small CPU low/mid soak term. It starts around
  `62 C`, fills around `71 C`, and is capped at `2-2.5%` so common CPU work can
  earn a slow airflow nudge without turning the high-temperature CPU override
  into an always-on low-band curve.
- Channel `6` remains explicitly blocked in the shipped live runtime policy.
- Do not assume one identical curve shape or RPM target across the three
  radiator Noctua lanes. Rear-radiator, front-radiator, and center-radiator
  tuning are expected to diverge.

## Failure Behavior

- After repeated missing primary temperature input for a channel, the loop logs
  `control_loop.sensor_failure_detected` and commands a safe full-speed setpoint
  for that channel. It logs `control_loop.sensor_recovered` when valid input
  returns.
- After repeated write failures for a channel, the loop logs
  `control_loop.circuit_breaker_opened` and stops trying that channel. The
  current reset path is process restart or a future explicit breaker-reset
  mechanism; do not assume automatic recovery from the open state.
- Sidecar failures are logged to `svg_mb_control_events.jsonl`; the loop avoids
  applying a write when it cannot record the pending-write sidecar first.

## Shutdown

On stop, `control-loop` restores each active channel back to its captured
baseline and removes its pending-write sidecar entry. A restore failure causes a
non-zero exit code. The shutdown path also appends restore/shutdown events to
`runtime\logs\svg_mb_control_events.jsonl`.

## Constraints

- The loop is direct-only.
- New feature work must not reintroduce external write helpers or bridge-style
  process ownership.
- Log chunk rotation and archive pruning remain product-owned here through
  `log_rotate_hours` and `log_retain_days`.
- Faster than `50 ms` control ticks, faster sensor polling, or additional live
  channels still require a fresh measurement gate pass through
  `docs\MEASUREMENT_GATE.md`.
