# SVG-MB-Control

`SVG-MB-Control` is the long-lived product-runtime repo for the SVG motherboard
stack.

Phase 0 in this repo is read-only and one-shot. It proves that a separate
native executable can consume the frozen Bench bridge through an external
process boundary.

Current external bridge contract reference:

- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\BRIDGE_CONTRACT.md`

## Current Phase 0 Scope

Implemented in this repo:

- one native executable: `svg-mb-control.exe`
- one subprocess adapter for `svg-mb-bench.exe read-snapshot`
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

Current Phase 0 seam:

1. launch `svg-mb-bench.exe read-snapshot`
2. capture stdout, stderr, and exit code
3. extract `snapshot_archive:` from Bench stdout
4. open the referenced snapshot JSON file
5. write that JSON to Control stdout

Bench does not currently emit the snapshot JSON directly on stdout.

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

Hermetic fake Bench path:

```powershell
build\x64-release\svg-mb-control.exe --bench-exe-path build\x64-release\fake-bench.exe
```

If `--bench-exe-path` is omitted, the binary probes sibling workspace patterns
rooted at its own executable directory and the current working directory.

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

`config/control.example.json` is a forward-declaration file for later phases.

Phase 0 documents these fields:

- `bench_exe_path`
- `poll_ms`
- `snapshot_path`

Phase 0 execution does not require config loading. The active input is
`--bench-exe-path`.

## Repo Boundary

- `SVG-MB-Control` owns the product runtime boundary.
- `SVG-MB-Bench` owns the current bridge commands, proof workflows, and artifact
  contracts.
- `SVG-MB-SIO` owns the reusable low-level Super I/O backend.
- `SVG-MB-LHM\LibreHardwareMonitor` remains a reserved reference tree and binary
  source, not the Control runtime boundary.
