# SVG-MB-Control

`SVG-MB-Control` is the long-lived product-runtime repo for the SVG motherboard
stack.

Phase 0 in this repo is read-only and subprocess-only. It proves that a
separate native executable can consume the frozen Bench bridge through an
external process boundary.

Current external bridge contract reference:

- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\BRIDGE_CONTRACT.md`

## Current Phase 0 Scope

Implemented in this repo:

- one native executable: `svg-mb-control.exe`
- one subprocess adapter for `svg-mb-bench.exe read-snapshot`
- one bounded subprocess adapter for `svg-mb-bench.exe logger-service`
- one hermetic fake Bench helper for subprocess tests
- one optional live integration lane against the real sibling Bench binary

Not implemented in this repo:

- control loop
- tray or UI
- `logger-service` supervision
- shared memory
- SQLite
- write-path consumption

## Consumption Model

This repo consumes Bench through an external subprocess only.

Phase 0 does not link against Bench and does not include Bench headers.

Current Phase 0 seams:

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

Bench does not emit the JSON payload directly on stdout. Control always opens
the JSON artifact file that Bench reports.

## Prerequisites

- Windows 10/11 x64
- Visual Studio 2022 with the C++ desktop workload
- CMake 3.21+
- Ninja

Optional live integration prerequisites:

- administrator privileges
- sibling `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\svg-mb-bench.exe`
- Bench PawnIO prerequisites satisfied on the machine

## Build

Manual Phase 0 build:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
```

Expected output:

- `build\x64-release\svg-mb-control.exe`
- `build\x64-release\fake-bench.exe`

## Run

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
build\x64-release\svg-mb-control.exe --config .\config\control.json
```

Hermetic fake Bench path:

```powershell
build\x64-release\svg-mb-control.exe --bench-exe-path build\x64-release\fake-bench.exe
```

Resolution precedence for the Bench executable is:

1. `--bench-exe-path`
2. `SVG_MB_CONTROL_BENCH_EXE`
3. `bench_exe_path` from the loaded control config
4. sibling workspace auto-resolution

Config resolution precedence is:

1. `--config`
2. `SVG_MB_CONTROL_CONFIG`
3. `config/control.json` near the executable or current working directory

Current bridge-command behavior:

- default: `logger-service`
- fallback: `read-snapshot`
- `--duration-ms` applies only to `logger-service`
- when `--duration-ms` is omitted, Control uses `poll_ms` from config and then
  falls back to `1000`

## Modes

The `--mode` flag selects the process lifetime:

- `one-shot` (default): Phase 0 behavior. Control launches Bench once, reads
  the snapshot JSON file, writes it to stdout, and exits.
- `read-loop`: Phase 1 behavior. Control spawns a persistent
  `logger-service` child and polls `snapshot_path` at `poll_ms`. Runs until
  Ctrl+C or Ctrl+Break, or until the child restart budget is exhausted.
- `write-once`: Phase 2 behavior. Control captures the current fan state
  baseline via `read-snapshot`, writes a pending-writes sidecar, spawns a
  bounded `set-fixed-duty` child, waits for exit, and clears the sidecar
  on clean exit. Requires `--write-channel`, `--write-pct`, and
  `--write-hold-ms` (or equivalents in the control config).

The `read-loop` mode requires a control config with `snapshot_path` set.

Regardless of mode, every Control startup reconciles
`pending_writes.json` in the runtime home before dispatching: for each
recorded entry, Control invokes `restore-auto` with the captured
baseline. A failed restore blocks startup with a non-zero exit code.

Example read-loop:

```powershell
build\x64-release\svg-mb-control.exe --mode read-loop --config .\config\control.json
```

Example write-once:

```powershell
build\x64-release\svg-mb-control.exe --mode write-once --config .\config\control.json --write-channel 3 --write-pct 60 --write-hold-ms 10000
```

## Runtime Home

In `read-loop` mode, Control writes to a runtime home directory it owns.

Resolution precedence:

1. `runtime_home_path` from the loaded control config
2. `runtime/` next to `svg-mb-control.exe`
3. `runtime/` under the current working directory

Contents:

- `control_runtime.json` — status file. Rewritten each poll cycle via
  temp-file + rename. Fields: `schema_version`, `status`, `status_detail`,
  `last_refresh`, `snapshot_source`, `restart_count`, `skipped_polls`,
  `successful_polls`, `stale`, `child_pid`.

`status` values: `running`, `shutdown`, `child-died`.

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

`config/control.example.json` remains the example file for the repo.

Phase 0 now supports loading a real `control.json` file.

Phase 0 documents these fields:

- `bench_exe_path`
- `poll_ms`
- `snapshot_path`

Phase 0 uses:

- `bench_exe_path` to resolve the sibling Bench binary
- `poll_ms` as the default bounded `logger-service` duration
- `snapshot_path` as an optional expected `current_state.json` path for the
  `logger-service` seam

## Repo Boundary

- `SVG-MB-Control` owns the product runtime boundary.
- `SVG-MB-Bench` owns the current bridge commands, proof workflows, and artifact
  contracts.
- `SVG-MB-SIO` owns the reusable low-level Super I/O backend.
- `SVG-MB-LHM\LibreHardwareMonitor` remains a reserved reference tree and binary
  source, not the Control runtime boundary.
