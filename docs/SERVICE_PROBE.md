# Service Probe

`svg-mb-control --service-probe` is a read-only feasibility probe for running
the controller as a Windows service (Session 0 / LocalSystem). It verifies the
capabilities a service account would need before the runtime is moved off the
current scheduled-task launcher. It performs no fan-duty writes.

## Invocation

```powershell
svg-mb-control --service-probe [--json] [--config <path>]
```

- `--config` resolves the runtime home and the runtime write policy the same
  way the long-running modes do. When omitted, the same default-config
  resolution as other commands applies.
- `--json` prints a schema-versioned report. Without `--json`, a plain-text
  report is printed.

To assess service feasibility, run the probe under the intended service
identity (for example via `PsExec -s` or a one-shot LocalSystem scheduled
task) and compare the result with an interactive run.

## Checks

| Check | Required | Pass condition |
|---|---|---|
| `amd_telemetry` | yes | `AmdReader` is available and returns an available sample |
| `gpu_telemetry` | no (advisory) | `GpuReader` is available and returns an available sample |
| `sio_fan_state` | yes | `ReadAllChannels()` on the SIO fan writer succeeds |
| `runtime_home_writable` | yes | a temp file is created and removed under the runtime home |
| `no_write_lifecycle` | yes | the fan writer opens and closes without creating `pending_writes.json` |

`gpu_telemetry` is advisory because this controller treats GPU input as
optional; a machine with no readable GPU telemetry can still run the service.
Only required checks affect the overall result.

The SIO check constructs the fan writer and calls `ReadAllChannels()` only.
`ApplyDuty` and `RestoreSavedState` are never called, so the probe cannot
change fan state.

## JSON report

Schema version `1`:

- `schema_version`
- `generated_time` — local ISO 8601
- `runtime_home`
- `overall` — `pass` or `fail`
- `checks` — array of `{ name, required, ok, detail }`

## Exit codes

- `0` — all required checks passed
- `1` — one or more required checks failed

## Scope

The probe does not start the control loop, write fan duties, install a
service, or modify the scheduled-task launcher. A passing probe is a
precondition for, not a commitment to, a future Windows-service runtime.
