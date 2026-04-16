# SVG-MB-Control

`SVG-MB-Control` is the standalone runtime repo for motherboard telemetry and
fan control. This repo owns the executable, packaged configs, runtime state,
vendored dependencies, and release artifacts. It does not depend on sibling
repos at runtime.

## Scope

Implemented here:

- `svg-mb-control.exe` as the only runtime executable
- direct AMD CPU telemetry
- optional direct NVIDIA telemetry through the vendored `gpu_telemetry` slice
- direct fan reads, writes, and restore through vendored `SVG-MB-SIO`
- direct `one-shot`, `read-loop`, `write-once`, and `control-loop`
- product-owned runtime files under `runtime\`
- hermetic smoke coverage through simulation environment hooks

Repo-local dependencies live under `third_party\` and `resources\`.

## Repo Boundary

- `SVG-MB-Control` owns process lifetime, config, policy, runtime state, and
  recovery behavior.
- `third_party\SVG-MB-SIO` is the low-level Super I/O backend.
- `third_party\nvapi-controller` contributes the vendored GPU telemetry slice.
- Legacy bridge executables are not part of the runtime contract in this repo.

## Build

Preferred release build:

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
- `release\runtime_policy_write_live.json`
- `release\resources\pawnio\AMDFamily17.bin`
- `release\resources\pawnio\LpcIO.bin`
- `release\build-info.json`
- `release\VERSION_TABLE.json`
- `release\archive\svg-mb-control-<timestamp>.zip`

## Run

Zero-arg packaged launch:

```powershell
cd .\release
.\svg-mb-control.exe
```

The packaged `control.json` sets `default_mode` to `read-loop`.

Direct one-shot snapshot to stdout:

```powershell
release\svg-mb-control.exe --mode one-shot
```

Direct read loop:

```powershell
release\svg-mb-control.exe --mode read-loop --config .\config\control.example.json
```

Direct write-once:

```powershell
release\svg-mb-control.exe --mode write-once --config .\config\control.example.json --write-channel 3 --write-pct 60 --write-hold-ms 10000
```

Direct control loop:

```powershell
release\svg-mb-control.exe --mode control-loop --config .\config\control.example.json
```

When `--mode` is omitted, Control uses `default_mode` from the loaded config.
If no config sets `default_mode`, Control falls back to `one-shot`.

Legacy bridge flags such as `--bridge-exe-path` are intentionally rejected in
this branch.

## Config

Canonical config files:

- `config\control.example.json` for repo-local editing
- `config\control.release.json` for packaging into `release\control.json`
- `config\runtime_policy_write_live.json` for the packaged live-write policy

Common fields:

- `default_mode`
- `snapshot_path`
- `runtime_policy_path`
- `runtime_home_path`
- `poll_ms`
- `baseline_freshness_ceiling_ms`
- `restore_timeout_ms`
- `control_loop`

Field notes:

- `snapshot_path` is optional. `read-loop` always writes
  `runtime\current_state.json` and mirrors the same payload to `snapshot_path`
  when configured.
- `runtime_policy_path` is read locally by direct write and control flows.
- Legacy bridge-era config keys such as `bridge_exe_path`,
  `bench_runtime_policy_path`, `logger_service_duration_ms`, and the old child
  restart / snapshot retry fields are rejected during config load.
- `config\control.example.json` is rooted at `config\`.
- `release\control.json` is copied to the release root, so its relative paths
  are root-relative instead of `config\`-relative.

## Runtime Home

Runtime-home resolution precedence:

1. `runtime_home_path` from the loaded config
2. `runtime\` next to `svg-mb-control.exe`
3. `runtime\` under the current working directory

Control writes:

- `current_state.json`
- `control_runtime.json`
- `pending_writes.json`

See `docs\RUNTIME_HOME.md` for field definitions.

## Tests

Run:

```powershell
python -m unittest discover tests -v
```

The smoke suite is direct-only. It launches the real executable and uses
simulation environment hooks for hermetic AMD and fan telemetry.

## Documentation

- `docs\CONTROL_LOOP.md`
- `docs\READ_LOOP.md`
- `docs\WRITE_ORCHESTRATION.md`
- `docs\RUNTIME_HOME.md`
