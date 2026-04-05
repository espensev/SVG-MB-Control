# Control Loop

Phase 3's persistent control loop. Selected with `--mode control-loop`.

## Lifecycle

1. Startup reconciliation runs (same as any other mode).
2. Control spawns a `svg-mb-bench.exe logger-service --duration-ms <N>`
   child via `BenchChildSupervisor`.
3. Control initializes `gpu_telemetry` if it was linked at build time.
   GPU sampling is optional; channels configured as `cpu_only` do not
   require GPU availability.
4. Main loop runs until Ctrl+C / Ctrl+Break. Each tick:
   - reads `snapshot_path` with retry
   - extracts CPU temperature from `amd_sensors` by the configured
     `cpu_temp_label` (default `Tctl/Tdie`)
   - samples GPU `core_c` and `memjn_c`; the GPU input is
     `max(core_c, memjn_c)`
   - per channel, blends temperatures per the configured `temp_blend`
     (`cpu_only`, `gpu_only`, `max_cpu_gpu`)
   - interpolates the per-channel curve, clamps to `[min_duty_pct, 100]`
   - if `|setpoint - last_issued| >= deadband_pct` and
     `(now - last_write_time) >= write_cooldown_ms`: issues a write
   - publishes extended `control_runtime.json` (see RUNTIME_HOME.md)
5. On Ctrl+Break, Control cancels all active `set-fixed-duty` children,
   clears their sidecar entries, and stops the `logger-service` child.

## Write orchestration

A new write cancels any prior active `set-fixed-duty` child on the same
channel via CTRL_BREAK_EVENT. The child's own console handler calls
`restore_auto(channel)` before exit, so there is a brief window where
the channel returns to motherboard auto mode between writes. A short
cooldown between writes is the operator's guardrail against
unnecessary channel churn.

## Baseline capture

Control captures `baseline_duty_raw` and `baseline_mode_raw` from the
first snapshot that contains the target channel, BEFORE issuing any
write. That baseline is written to every subsequent sidecar entry for
the channel, so crash recovery restores to the pre-Control state.

## Config shape

The control config must include a `control_loop` object with a
non-empty `channels` array. Example:

```json
{
  "schema_version": 4,
  "bench_exe_path": "...",
  "snapshot_path": "...",
  "runtime_home_path": "...",
  "control_loop": {
    "poll_tick_ms": 2000,
    "write_cooldown_ms": 10000,
    "deadband_pct": 3.0,
    "control_hold_ms": 60000,
    "cpu_temp_label": "Tctl/Tdie",
    "logger_service_duration_ms": 86400000,
    "channels": [
      {
        "channel": 2,
        "temp_blend": "max_cpu_gpu",
        "min_duty_pct": 45.0,
        "curve": [
          { "temp_c": 30, "duty_pct": 45 },
          { "temp_c": 55, "duty_pct": 55 },
          { "temp_c": 70, "duty_pct": 70 },
          { "temp_c": 85, "duty_pct": 90 },
          { "temp_c": 95, "duty_pct": 100 }
        ]
      }
    ]
  }
}
```

Curves are sorted by `temp_c` at load. Temperatures below the first
point clamp to the first duty; temperatures above the last point clamp
to the last duty.

## Runtime policy requirement

`set-fixed-duty` writes require Bench's runtime policy
`writes_enabled: true`. Set the environment variable
`SVG_MB_RUNTIME_POLICY` before launching control-loop mode, pointing
at a policy file with writes enabled. Without this, all control-loop
writes will be refused by Bench with exit code 2, and the sidecar will
be cleared per the documented exit-code rule.

## What this phase deliberately skips

- no ARC overlay (EMA, confidence, integral) — Phase 4
- no single-instance enforcement — Phase 4
- no tray / service wrapping — Phase 4
- no dynamic curve reload — curves are loaded once at start
- no per-channel policy runtime override
- no RPM feedback into the setpoint
