# SVG-MB-Control

`SVG-MB-Control` is the long-lived product-runtime repo for the SVG motherboard
stack.

Current external contract reference:

- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\BRIDGE_CONTRACT.md`

## Current Scope

Implemented in this repo:

- one native runtime executable: `svg-mb-control.exe`
- subprocess-only Bench consumption for `logger-service` and `read-snapshot`
- bounded write orchestration for `set-fixed-duty` and `restore-auto`
- persistent `read-loop` and `control-loop` process lifetimes
- product-owned runtime home and pending-write reconciliation
- optional in-process GPU telemetry through linked `gpu_telemetry`
- hermetic subprocess tests through `fake-bench.exe`

Still out of scope:

- tray or UI
- shared memory
- SQLite

## Boundaries

Control consumes Bench through an external subprocess only. It does not include
Bench headers and does not link Bench sources.

Read seams:

1. default path:
   1. launch `svg-mb-bench.exe logger-service --duration-ms <poll_ms>`
   2. capture stdout, stderr, and exit code
   3. extract `snapshot_path:` from Bench stdout
   4. open the refreshed `current_state.json`
   5. write that JSON to Control stdout
2. fallback path:
   1. launch `svg-mb-bench.exe read-snapshot`
   2. extract `snapshot_archive:` from Bench stdout
   3. open the referenced snapshot JSON file
   4. write that JSON to Control stdout

Write seams:

- `set-fixed-duty` for bounded write requests
- `restore-auto` for startup reconciliation and shutdown recovery

Bench does not emit the JSON payload directly on stdout. Control always opens
the JSON artifact file that Bench reports.

GPU telemetry is in-process through the linked `gpu_telemetry` package from
`D:\Development\Thermals\Nvida-fancontrol-unofficial\nvapi-controller`. Control
does not spawn a separate GPU controller process for telemetry.

The runtime path is native C++ only. Python is used for tests and release-time
verification, not by `svg-mb-control.exe`.

## Prerequisites

- Windows 10/11 x64
- Visual Studio 2022 with the C++ desktop workload
- CMake 3.21+
- Ninja

Optional live prerequisites:

- administrator privileges
- sibling `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\svg-mb-bench.exe`
- Bench PawnIO prerequisites satisfied on the machine
- optional `gpu_telemetry` package from `D:\Development\Thermals\Nvida-fancontrol-unofficial\nvapi-controller`

## Build

Release build:

```powershell
.\build-release.ps1
```

Useful options:

- `-KeepBuildDir` keeps `build\` after a successful release build
- `-SkipTests` skips `python -m unittest discover tests -v`

Manual CMake build:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
```

Release-script outputs:

- `release\svg-mb-control.exe`
- `release\control.json`
- `release\build-info.json`
- `release\VERSION_TABLE.json`
- `release\archive\svg-mb-control-<timestamp>.zip`

Manual-build outputs:

- `build\x64-release\svg-mb-control.exe`
- `build\x64-release\fake-bench.exe`

## Run

Zero-arg packaged launch:

```powershell
cd .\release
.\svg-mb-control.exe
```

The packaged release ships `control.json` beside the exe. That config sets
`default_mode` to `control-loop`, so zero-arg launch starts the control loop.

Explicit sibling Bench path:

```powershell
build\x64-release\svg-mb-control.exe --bench-exe-path ..\SVG-MB-Bench\svg-mb-bench.exe
```

Explicit read-snapshot fallback:

```powershell
build\x64-release\svg-mb-control.exe --bridge-command read-snapshot --bench-exe-path ..\SVG-MB-Bench\svg-mb-bench.exe
```

Explicit config path:

```powershell
build\x64-release\svg-mb-control.exe --config .\config\control.example.json
```

Hermetic fake Bench path:

```powershell
build\x64-release\svg-mb-control.exe --bench-exe-path build\x64-release\fake-bench.exe
```

Bench executable resolution precedence:

1. `--bench-exe-path`
2. `SVG_MB_CONTROL_BENCH_EXE`
3. `bench_exe_path` from the loaded control config
4. sibling workspace or co-packaged auto-resolution

Config resolution precedence:

1. `--config`
2. `SVG_MB_CONTROL_CONFIG`
3. `control.json` or `config\control.json` near the executable or current working directory

Bridge-command behavior:

- default: `logger-service`
- fallback: `read-snapshot`
- `--duration-ms` applies only to `logger-service`
- when `--duration-ms` is omitted, Control uses `poll_ms` from config and then
  falls back to `1000`

## Modes

- `one-shot`: launch Bench once, load the reported JSON artifact, write it to
  stdout, and exit
- `read-loop`: supervise a persistent `logger-service` child and poll
  `snapshot_path` until Ctrl+C or Ctrl+Break
- `write-once`: capture a baseline, issue one bounded `set-fixed-duty`, and
  reconcile restore state on exit
- `control-loop`: supervise `logger-service`, sample CPU and optional GPU
  temperatures, evaluate per-channel curves, and issue bounded writes when the
  deadband and cooldown rules allow it

When `--mode` is omitted, Control uses `default_mode` from the loaded config.
If the config does not set `default_mode`, Control falls back to `one-shot`.

Regardless of mode, every Control startup reconciles `pending_writes.json` in
the runtime home before dispatching. A failed restore blocks startup with a
non-zero exit code.

Example read-loop:

```powershell
build\x64-release\svg-mb-control.exe --mode read-loop --config .\config\control.example.json
```

Example write-once:

```powershell
build\x64-release\svg-mb-control.exe --mode write-once --config .\config\control.example.json --write-channel 3 --write-pct 60 --write-hold-ms 10000
```

Example control-loop:

```powershell
build\x64-release\svg-mb-control.exe --config .\config\control.example.json
```

`config\control.example.json` carries a full control-loop config for a
6-channel NCT6701D on ROG STRIX X870-F with an RTX 5090 and sets
`default_mode` to `control-loop`.

`bench_runtime_policy_path` makes Control export `SVG_MB_RUNTIME_POLICY` to its
own environment before spawning Bench children, so a live write loop does not
need a separate env var at launch time. An explicit env var already set by the
operator still takes precedence.

Diagnostic flag:

```powershell
build\x64-release\svg-mb-control.exe --diagnose-gpu
```

## Runtime Home

Runtime-home resolution precedence:

1. `runtime_home_path` from the loaded control config
2. `runtime\` next to `svg-mb-control.exe`
3. `runtime\` under the current working directory

Control writes:

- `control_runtime.json` with current loop status
- `pending_writes.json` for restore reconciliation

## Tests

Run:

```powershell
python -m unittest discover tests -v
```

Test lanes:

- hermetic subprocess lane: always-on, uses `fake-bench.exe`
- optional live integration lane: uses the real sibling Bench exe and skips when
  elevation or prerequisites are missing

The hermetic lane exercises the real `CreateProcessW` path. It does not use a
batch-file stub.

## Config

Canonical config files:

- `config\control.example.json` for repo-local editing
- `release\control.json` for zero-arg packaged launch

Key fields:

- `default_mode`
- `bench_exe_path`
- `snapshot_path`
- `bench_runtime_policy_path`
- `runtime_home_path`
- `poll_ms`
- `control_loop`

## Repo Boundary

- `SVG-MB-Control` owns the product runtime boundary
- `SVG-MB-Bench` owns the bridge commands, proof workflows, and artifact contracts
- `SVG-MB-SIO` owns the reusable low-level Super I/O backend
- `SVG-MB-LHM\LibreHardwareMonitor` remains a reserved reference tree and binary source, not the Control runtime boundary
