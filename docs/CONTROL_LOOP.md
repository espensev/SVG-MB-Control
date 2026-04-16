# Control Loop

## Purpose

`control-loop` is the long-running direct control path. It samples temperatures
and fan state in-process, calculates channel setpoints from config curves, and
applies writes through `SVG-MB-SIO`.

## Inputs

Top-level config fields used by `control-loop`:

- `runtime_home_path`
- `runtime_policy_path`
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
4. Capture the baseline duty and mode for each configured channel.
5. Blend temperatures according to `temp_blend`.
6. Interpolate the configured curve and clamp with `min_duty_pct`.
7. Skip writes blocked by deadband, cooldown, or runtime policy.
8. Record a pending-write sidecar entry before each applied write.
9. Restore the captured baseline once the hold window expires or shutdown is requested.

## Outputs

`control-loop` writes:

- `runtime\current_state.json`
- `runtime\control_runtime.json`
- `runtime\pending_writes.json` while a write is active

`control_runtime.json` includes loop-level counters plus per-channel totals and
last observed values.

## Policy Behavior

`runtime_policy_path` is resolved locally inside Control.

- `writes_enabled=false` blocks writes globally.
- `blocked_channels` blocks specific channels.
- The published fan payload exposes `write_allowed`, `policy_blocked`, and
  `effective_write_allowed`.

## Shutdown

On stop, `control-loop` restores each active channel back to its captured
baseline and removes its pending-write sidecar entry. A restore failure causes a
non-zero exit code.

## Constraints

- The loop is direct-only.
- New feature work must not reintroduce external write helpers or bridge-style
  process ownership.
