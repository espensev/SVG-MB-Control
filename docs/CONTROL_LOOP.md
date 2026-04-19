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

## Runtime Flow

1. Resolve config, runtime home, and runtime policy.
2. Initialize the direct fan backend.
3. On each tick, sample AMD, GPU, and fan telemetry in-process.
4. Append the sampled row to the active CSV chunk and mirror it to the fixed
   live CSV path.
5. Capture the baseline duty and mode for each configured channel.
6. Blend temperatures according to `temp_blend`.
7. Interpolate the configured curve and clamp with `min_duty_pct`.
8. Skip writes blocked by deadband, cooldown, or runtime policy.
9. Record a pending-write sidecar entry before each applied write.
10. Append durable JSONL events for loop start, rotations, baseline capture,
    writes, restores, and failures.
11. Restore the captured baseline once the hold window expires or shutdown is requested.

## Outputs

`control-loop` writes:

- `runtime\current_state.json`
- `runtime\control_runtime.json`
- `runtime\pending_writes.json` while a write is active
- `runtime\logs\svg_mb_control_output.csv`
- `runtime\logs\archive\svg_mb_control_control-loop_<timestamp>.csv`
- `runtime\logs\svg_mb_control_events.jsonl`

`control_runtime.json` includes loop-level counters, the active log paths, and
per-channel totals plus last observed values.

## Policy Behavior

`runtime_policy_path` is resolved locally inside Control.

- `writes_enabled=false` blocks writes globally.
- `blocked_channels` blocks specific channels.
- The published fan payload exposes `write_allowed`, `policy_blocked`, and
  `effective_write_allowed`.

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
- Faster poll or write behavior should not be tuned further until the
  measurement gate in `docs\MEASUREMENT_GATE.md` is completed.
