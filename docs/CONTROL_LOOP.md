# Control Loop

## Purpose

`control-loop` is the long-running direct control path. It samples temperatures
and fan state in-process, calculates channel setpoints from config curves, and
applies writes through `SVG-MB-SIO`.

`docs\CONTROL_PIPELINE_MATH.md` is the maintained numerical reference for the
per-tick computation: curve lookup, smoothing, boost composition, low-band
behavior, cadence scoring, and CSV/status identities. Update it with this file
whenever the control computation or its telemetry fields change.

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

Optional loop-level fields:

- `poll_tick_floor_ms`: fastest tick interval the loop may use under thermal
  transients. Defaults to `poll_tick_ms`, which disables adaptation (the loop
  then behaves identically to a fixed `poll_tick_ms`). When set below
  `poll_tick_ms` it must be `>= 25` and `<= poll_tick_ms`; the loop shortens
  the interval toward this floor as CPU/GPU temperature slew rises and relaxes
  it back toward `poll_tick_ms` at `cadence_relax_per_s`. See
  `docs/adaptive-cadence-design-2026-05-19.md`.
- `cadence_slew_start_c_per_s`: temperature slew (default `0.5`) below which
  cadence does not tighten.
- `cadence_slew_full_c_per_s`: temperature slew (default `3.0`) at or above
  which cadence tightens fully. Must be greater than
  `cadence_slew_start_c_per_s` when adaptation is enabled.
- `cadence_relax_per_s`: rate the effective interval relaxes back toward
  `poll_tick_ms`. Defaults to `(poll_tick_ms - poll_tick_floor_ms) / 3`.

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
- `source_aware_cpu_hot_guard_c`: optional CPU/Tctl guard for
  `max_cpu_gpu_source_aware`. Below this guard, the primary curve is evaluated
  from the GPU envelope and the CPU can still win through `cpu_override_curve`.
  At or above this guard, the channel falls back to legacy raw
  `max_cpu_gpu` selection for high-CPU protection.
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
- `midband_pressure_start_c`: optional smootherstep pressure-boost threshold
  for the mid-temperature band
- `midband_pressure_full_c`: temperature where the mid-band pressure boost
  accumulates at its full configured rise rate
- `midband_pressure_rise_pct_per_sec`: mid-band boost accumulation rate
- `midband_pressure_fall_pct_per_sec`: mid-band boost decay rate
- `midband_pressure_max_boost_pct`: maximum extra duty from mid-band pressure
- `gpu_airflow_start_c`: optional GPU-envelope threshold for early airflow
- `gpu_airflow_full_c`: GPU-envelope temperature where the airflow boost
  accumulates at its full configured rise rate
- `gpu_airflow_rise_pct_per_sec`: GPU airflow boost accumulation rate
- `gpu_airflow_fall_pct_per_sec`: GPU airflow boost decay rate
- `gpu_airflow_max_boost_pct`: maximum extra duty from GPU airflow
- `cpu_low_soak_start_c`: optional low/medium CPU sustained-heat threshold
- `cpu_low_soak_full_c`: CPU temperature where the low-soak term accumulates at
  its full configured rise rate
- `cpu_low_soak_release_c`: CPU temperature at or below which the low-soak term
  decays
- `cpu_low_soak_rise_pct_per_min`: low-soak accumulation rate while CPU remains
  in the configured band
- `cpu_low_soak_fall_pct_per_min`: low-soak decay rate
- `cpu_low_soak_max_boost_pct`: maximum extra duty from low/medium CPU soak

Optional loop-level response override:

- `low_band_residual_cap_pct`: caps how much of the raw low-band stage boost
  contributes to the final setpoint. Runtime CSV/status publish both the raw
  low-band stage value and the effective capped value.

Config loading validates basic ranges and required curve/channel structure. A
bad control config should fail at startup instead of producing undefined fan
behavior.

`smootherstep` uses the standard quintic blend `t^3 * (6t^2 - 15t + 10)`,
evaluated in Horner form in the curve lookup path.

## Runtime Flow

1. Resolve config, runtime home, and runtime policy.
2. Initialize the direct fan backend.
3. On each tick, sample AMD, GPU, and fan telemetry in-process.
4. Append the sampled row to the active CSV chunk and refresh the fixed live
   CSV mirror on the configured flush interval.
5. Capture the baseline duty and mode for each configured channel.
6. Select the primary curve temperature according to `temp_blend`. For
   `max_cpu_gpu_source_aware`, use the GPU envelope below the configured CPU
   guard and legacy CPU/GPU max selection at or above the guard.
7. Interpolate the configured curve and clamp with `min_duty_pct`.
8. If `cpu_override_curve` is present and CPU telemetry is available,
   interpolate it against CPU/Tctl and use the higher duty.
9. Smooth the raw demand and apply bounded decay, then add any configured
   thermal-pressure, mid-band pressure, GPU airflow, CPU low-soak, and
   low-band stage boost before the normal rate limiter. This allows
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
- `cadence_transient`

`loop_achieved_interval_ms` is measured between tick starts. Each tick targets
`loop_intended_interval_ms`: this equals `poll_tick_ms` when adaptive cadence
is off (`poll_tick_floor_ms` absent or `== poll_tick_ms`), and otherwise the
effective interval in `[poll_tick_floor_ms, poll_tick_ms]` derived from
`cadence_transient` (a unitless `[0, 1]` slew score; `0` means no transient).
`loop_slip_ms` and `loop_overrun` remain relative to the configured
`poll_tick_ms`.

The process CPU and memory fields are emitted in both `control_runtime.json` and
the control-loop CSV so fast polling/write profiles can be watched for resource
cost. CPU percent is a rolling roughly one-second process average; memory fields
are sampled on each loop row.

Each controlled channel also publishes `last_thermal_pressure_boost_pct`,
`last_midband_pressure_boost_pct`, `last_gpu_airflow_boost_pct`, and
`last_cpu_low_soak_boost_pct` in `control_runtime.json`, with matching
`channelN_*_boost_pct` columns in the control-loop CSV. These values are slow
leaky-integral terms added on top of the base curve/EMA demand.

The control-loop CSV also includes `channelN_feedforward_pct` and
`channelN_correction_pct`. Feedforward is the raw curve/overlay demand before
the control loop applies smoothing, thermal pressure, and rate limits;
correction is the final setpoint minus that feedforward term.

Primary temperature attribution is emitted as `last_primary_temp_source` in
`control_runtime.json` and `channelN_primary_temp_source` in the CSV. Current
sources are `cpu`, `gpu`, `cpu_fallback`, `cpu_guard`, `gpu_guard`, and
`unavailable`. `cpu_fallback` means a source-aware channel used CPU because
GPU telemetry was unavailable below the CPU guard.

Response attribution is emitted as `last_response_source` in
`control_runtime.json` and `channelN_response_source` in the CSV. Current
sources are `primary_curve`, `cpu_override`, `sensor_safe_mode`,
`source_aware_cpu_dropout_safe_mode`, and optional
modifiers such as `+thermal_pressure`, `+midband_pressure`, and
`+gpu_airflow`, `+cpu_low_soak`, and `+low_band_stage`. CSV rows also include
`channelN_write_reason`, which is `first_write`, `setpoint_delta`,
`authority_reassert`, `write_failed`, or `none` for ticks without a write.

The current status JSON also publishes `last_raw_demand_pct` and
`last_smoothed_demand_pct` so a live reader can distinguish the curve/overlay
demand from the smoothed demand and final rate-limited setpoint.

Per-channel failure state is also surfaced in `control_runtime.json`:
`sensor_failed`, `consecutive_sensor_failures`, `circuit_breaker_open`,
`consecutive_write_failures`, and `consecutive_sidecar_persist_failures`. The JSON
remains rate-limited; use the event log for exact transition timing.

## Lifecycle Commands

With the packaged `control.json`, a zero-argument launch or `--start` starts a
hidden supervisor and returns the shell prompt. The supervisor launches the
worker with `--run-foreground`, writes supervisor/worker stdout and stderr logs
in the runtime home, and restarts the worker after an unexpected non-zero exit.
Startup/config failures are reported immediately instead of entering a restart
loop.

The normal Windows install path is
`Install-SVG-MB-ControlScheduledTask.ps1`. It registers a scheduled task named
`SVG-MB Control` for system startup and current-user logon, runs it elevated,
and starts the controller immediately. The task action invokes
`svg-mb-control-task-runner.exe --start --config <release\control.json>`, so
startup and logon launches use the same supervised path as a manual operator
start.

Operator commands use the same runtime-home resolution as the active config:

- `--status` reads `control_runtime.json` and checks `process_id`.
- `--status --json` or `--health --json` emits the machine-readable health
  contract with exit codes `0=healthy`, `1=degraded`, `2=stale/stopped`, and
  `3=failed`.
- `--stop` writes `stop.request.json`; the worker exits through the normal
  restore/shutdown path.
- `--restart` performs the same cooperative stop and only launches a new
  supervisor after the previous worker reports stopped.
- `--reset-breakers` writes `circuit_breaker_reset.request.json`; the
  control-loop consumes it on the next tick and clears open write-failure
  breakers. Add `--reset-breaker-channel <n>` to target one channel.

`Install-SVG-MB-ControlWatchdogScheduledTask.ps1` adds a separate watchdog task
through `svg-mb-control-task-runner.exe`. The task checks health at logon and
every minute. It restarts only `stale` or `stopped` states; `degraded` is
visible but not restarted, and `failed` is left for operator review.

Passing `--mode control-loop --config <path>` keeps the loop attached to the
current terminal and does not add supervisor restart behavior.

## Policy Behavior

`runtime_policy_path` is resolved locally inside Control.

- `writes_enabled=false` blocks writes globally.
- `blocked_channels` blocks specific channels.
- The published fan payload exposes `write_allowed`, `policy_blocked`, and
  `effective_write_allowed`.
- The packaged live control policy controls Channels `0,1,2,3,4,5`.
- The packaged loop cadence is `poll_tick_ms=250` with `write_cooldown_ms=250`
  and a sub-one-percent deadband. The goal is to preserve authority and emit
  small intermediate PWM steps rather than audible multi-second staircases.
- While `control-loop` runs, Control requests a 1 ms Windows timer period so the
  fixed-start-period loop is not stretched by the default scheduler
  quantum.
- The SND-DESK fan topology and pressure strategy are maintained in
  `docs\COOLING_STRATEGY.md` and
  `config\machines\snd-desk.cooling.policy.json`. In brief, channels
  `2,3` are the PA602 stock 200 mm front intakes, channel `4` is the
  front radiator Noctua intake, and channel `6` remains out of the
  shipped live loop.
- Channels `2`, `3`, and `4` use lower hard minima plus low/medium curve
  points through `72 C` for the positive-pressure intake bias. The curve and
  CPU overlay carry this demand; the loop has no separate soft-floor operator.
- The shipped curves use GPU envelope as the primary case-airflow signal and a
  mandatory CPU/Tctl overlay so Cinebench plus max CUDA load can raise channels
  even when the GPU curve alone would not.
- The radiator Noctua lanes `1,4,5` intentionally use different CPU override
  points and decay rates so they do not ramp as one synchronized block.
- The same radiator lanes also carry a slow thermal-pressure boost. Sustained
  high heat can add duty even when the instantaneous curve/EMA would otherwise
  hover too low or drop during small temperature dips.
- Lanes `1,4,5` also carry a small CPU low/mid soak term. In the shipped
  configs it starts at `72 C`, fills at `82 C`, releases at `68 C`, and is
  capped at `0.3%` so common CPU work can earn a slow airflow nudge without
  turning the high-temperature CPU override into an always-on low-band curve.
- Channel `6` remains explicitly blocked in the shipped live runtime policy.
- Do not assume one identical curve shape or RPM target across the three
  radiator Noctua lanes. Rear-radiator, front-radiator intake, and
  center-radiator tuning are expected to diverge.

## Failure Behavior

- After repeated missing primary temperature input for a channel, the loop logs
  `control_loop.sensor_failure_detected` and commands a safe full-speed setpoint
  for that channel. It logs `control_loop.sensor_recovered` when valid input
  returns.
- On a `max_cpu_gpu_source_aware` channel, a CPU-input dropout (CPU previously
  available, now unavailable while GPU remains) counts toward the same
  three-miss sensor-failure trip rather than being masked by the GPU fallback;
  the trip uses the response source `source_aware_cpu_dropout_safe_mode` and the
  existing `sensor_failure_detected` / `sensor_recovered` events (FEAT-0013).
  Below the threshold the channel keeps cooling on the GPU curve.
- After repeated write failures for a channel, the loop logs
  `control_loop.circuit_breaker_opened` and stops normal writes for that
  channel. A sensor-safe (safe-mode) command still bypasses the open breaker so
  a thermal-safety write reaches the hardware
  (`docs/discovery-recovery-gap-audit-2026-06-04.md`, remediation 3); a
  successful bypassed write closes the breaker. The
  reset path is `--reset-breakers` or
  `--reset-breakers --reset-breaker-channel <n>`. The reset clears
  `circuit_breaker_open` and `consecutive_write_failures`, logs
  `control_loop.circuit_breaker_reset`, and leaves policy/fan-write gates in
  place; if the underlying write failure remains, the breaker can open again.
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
- Faster than the shipped `250 ms` control/write profile, faster sensor polling,
  adaptive cadence floors below the shipped profile, or additional live channels
  still require a fresh measurement gate pass through
  `docs\MEASUREMENT_GATE.md`.
