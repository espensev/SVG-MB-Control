# Build Targets and Dependencies

Maps the build targets, shipped executables, runtime processes, and vendored
dependencies of `SVG-MB-Control`. Use this alongside `docs\CODE_MAP.md`
(per-file responsibilities) and `docs\STRUCTURE_AND_STABILITY.md` (module
boundaries). `AGENTS.md` remains the canonical agent contract.

`SVG-MB-Control` is a standalone Windows x64 C++ application for motherboard,
CPU, and GPU telemetry and fan control on the NCT6701D Super-I/O family with
AMD Family 17h/19h/1Ah CPUs (Zen2 through Zen5). It owns process lifetime, config, control policy, runtime
state, and recovery, and does not depend on sibling repos at runtime.

All targets below are produced by `.\build-release.ps1` (release publish) or
`.\scripts\Test-LocalCI.ps1 -KeepBuildDir` (no-publish CI), or the manual
`cmake --preset x64-release` path. Test targets build only under
`BUILD_TESTING` and run via CTest. See `README.md` and `AGENTS.md` for the
build and validation workflow.

## Library Target

- `svg_mb_control_core` — static library holding non-app implementation code
  from `src\control\`, `src\runtime\`, `src\hardware\`, `src\platform\` (every
  file except `src\platform\task_runner.cpp`, which builds only into the
  task-runner executable), `src\policy\`, and `src\analyze\`. The core source
  list in `CMakeLists.txt` is an explicit file list, not a directory glob. The
  shipped executable is a thin wrapper over this target so C++ unit tests
  exercise core behavior without launching a full process. Built with
  `SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED=1` when the `SVG_MB_CONTROL_ENABLE_GPU`
  option is ON (its default) and the vendored gpu_telemetry slice under
  `third_party\nvapi-controller\telemetry` is present, which makes the
  `gpu_telemetry::gpu_telemetry` target available.

## Executables

CMake target names map to release output names via `OUTPUT_NAME`.

### Shipped

- `svg_mb_control` -> `svg-mb-control.exe` — the only executable that links
  `svg_mb_control_core` and hosts the telemetry, control, and runtime logic.
  `--mode` selects `one-shot`, `read-loop`, `write-once`, `control-loop`,
  `calibrate`, or `evidence-log`. It also accepts the `analyze` positional
  subcommand (`analyze ingest|prune|report`, dispatched before option parsing)
  and standalone flags including `--start`, `--status`, `--health`,
  `--show-config`, `--stop`, `--restart`, `--reset-breakers`, `--diagnose-amd`,
  and `--diagnose-gpu`. (`--watchdog-run` is a flag of the task-runner
  executable below, not of `svg-mb-control.exe`.)
- `svg_mb_control_task_runner` -> `svg-mb-control-task-runner.exe`
  (Windows-only, `WIN32` subsystem) — windowless launcher built from
  `src\platform\task_runner.cpp`, used by both scheduled tasks to start and
  supervise `svg-mb-control.exe`. Links only `nlohmann_json::nlohmann_json`,
  not the core.

### Test executables

Built only under `BUILD_TESTING` and registered with CTest. They are not part
of the release package. Each links `svg_mb_control_core`, except the two
header-only tests (`svg_mb_control_rapl_energy_tests`,
`svg_mb_control_cpu_cycles_tests`), which deliberately do not link the core
library because the `rapl_energy.h` / `cpu_cycles.h` math they cover is
dependency-free.

- `svg_mb_control_core_tests` — `tests\cpp\core_smoke_tests.cpp`
- `svg_mb_control_pawnio_binary_tests` — `tests\cpp\pawnio_binary_tests.cpp`
- `svg_mb_control_boost_stage_tests` — `tests\cpp\boost_stage_tests.cpp`
- `svg_mb_control_math_tests` — `tests\cpp\control_math_tests.cpp`
- `svg_mb_control_analyze_report_tests` — `tests\cpp\analyze_report_tests.cpp`
- `svg_mb_control_loop_config_tests` — `tests\cpp\control_loop_config_tests.cpp`
- `svg_mb_control_csv_rows_tests` — `tests\cpp\csv_rows_tests.cpp`
- `svg_mb_control_channel_write_tests` — `tests\cpp\channel_write_tests.cpp`
- `svg_mb_control_force_terminate_tests` —
  `tests\cpp\worker_force_terminate_tests.cpp`
- `svg_mb_control_amd_decode_tests` — `tests\cpp\amd_decode_tests.cpp`
- `svg_mb_control_power_anticipation_tests` —
  `tests\cpp\power_anticipation_tests.cpp` (covers the design-support
  `src\control\power_anticipation.h`; core-linked for `SmoothStep`)
- `svg_mb_control_rapl_energy_tests` — `tests\cpp\rapl_energy_tests.cpp`
- `svg_mb_control_cpu_cycles_tests` — `tests\cpp\cpu_cycles_tests.cpp`

## Operator Helper Tools

These tools are not CMake targets and are not part of the release package.

- `tools\profile_switch_ui` (`svg-mb-profile-ui`) — a small Rust local HTTP UI
  for profile switching. It is run manually through
  `scripts\Start-ProfileSwitchUi.ps1`, binds to `127.0.0.1` by default, lists
  known profile JSON files, shows `--health` / `--status`, and applies a profile
  by invoking the shipped `svg-mb-control.exe --set-profile <name>` operator
  path. It adds no runtime protocol and does not participate in scheduled-task
  process lifetime.

## Runtime Processes

Both processes run as Windows Scheduled Tasks under task path
`\SVG-MB Control\` (names defined in `Install-SVG-MB-ControlCommon.ps1`).

- `SVG-MB Control` — the long-running control process. The scheduled-task
  action runs `svg-mb-control-task-runner.exe --start --config "<path>"`
  (`Install-SVG-MB-ControlScheduledTask.ps1`); the task-runner then launches
  `svg-mb-control.exe --start --config <path>` (`src\platform\task_runner.cpp`),
  which starts the long-running mode named in the config (`read-loop` or
  `control-loop`; the shipped config uses `control-loop`). Installed by
  `Install-SVG-MB-ControlScheduledTask.ps1` with system-startup and
  current-user logon triggers.
- `SVG-MB Control Watchdog` — recovery task whose scheduled-task action runs
  `svg-mb-control-task-runner.exe --watchdog-run --config "<path>"`
  (`Install-SVG-MB-ControlWatchdogScheduledTask.ps1`). On `--watchdog-run`, the
  task-runner runs `svg-mb-control.exe --health --json` (with `--config <path>`
  appended by the runner) and, when health status is `2`, runs
  `svg-mb-control.exe --restart` (`src\platform\task_runner.cpp`). Installed by
  `Install-SVG-MB-ControlWatchdogScheduledTask.ps1`. (The script's `-Status`
  path separately runs `svg-mb-control.exe --health --json --config <path>`
  only to print health; it is not the registered task action.)

## Vendored Dependencies

All release inputs are repo-owned and pinned under `third_party\` and
`resources\`; the normal release configure path does not fetch from the
network. The top-level `CMakeLists.txt` raises `FATAL_ERROR` when the
`nlohmann-json`, `sqlite3`, or `SVG-MB-SIO` vendored trees are missing. When
`SVG_MB_CONTROL_ENABLE_GPU` is ON (the default) and the
`nvapi-controller\telemetry` tree is missing, the configure step instead emits
`message(WARNING ...)` and builds with GPU input disabled. The
`third_party\pawnio` tree is documentation only and is not referenced by
`CMakeLists.txt`.

- `third_party\SVG-MB-SIO` — static C++ library (`svg_mb_sio.lib`, target
  `svg_mb_sio::svg_mb_sio`) for NCT6701D Super-I/O access: fan reads, writes,
  restore, SIO voltage and temperature reads, and raw register access. It is
  SIO-only with no AMD SMN transport, no policy, and no process lifetime, and
  is built in-tree through `add_subdirectory`. Live hardware access needs the
  PawnIO driver and `LpcIO.bin`.
- `third_party\nvapi-controller` — vendored NVIDIA telemetry source, including
  the `gpu_telemetry` slice (`gpu_probe.h`, `gpu_sensor_reader.h`, NVAPI and
  NVML loaders). Supplies optional direct NVIDIA fan, thermal, and clock
  telemetry, gated by `SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED`. Only the
  `telemetry\` subdir is pulled into the build via `add_subdirectory`.
- `third_party\pawnio` — provenance and license documentation only
  (LGPL-2.1-or-later, namazso PawnIO.Modules release `0.2.6`). The driver
  module binaries ship under `resources\pawnio\` (`AMDFamily17.bin`,
  `LpcIO.bin`). Only `AMDFamily17.bin` has a recorded SHA-256
  (`kPawnIoSpecAmdFamily17V1`, `src\hardware\pawnio_binary.cpp`), checked
  warn-only — a mismatch logs a warning and the binary still loads.
  `LpcIO.bin` is loaded without a hash check
  (`third_party\SVG-MB-SIO\src\fan_sio.cpp`). They provide LPC/SIO and AMD CPU
  register access.
- `third_party\sqlite3` — upstream SQLite amalgamation (`sqlite3.c`,
  `sqlite3.h`), compiled as the static C target `sqlite3_amalgamation`
  (alias `sqlite3::sqlite3`); under MSVC it is built with `/W0`.
- `third_party\nlohmann-json` — single-header JSON, exposed as the INTERFACE
  target `nlohmann_json::nlohmann_json`.

## Drivers and Hardware Access

Live telemetry and fan control go through one kernel driver: **PawnIO**
(upstream namazso PawnIO; the `resources\pawnio\` modules come from
PawnIO.Modules `0.2.6`). The app opens the device object
`\\?\GLOBALROOT\Device\PawnIO` with `CreateFileA(GENERIC_READ | GENERIC_WRITE)`
and does not install or start the driver: it assumes PawnIO is already
installed and running, and retries the open up to 16 times with exponential
backoff (25 ms initial, 250 ms cap) before giving up
(`src\hardware\amd_reader.cpp`). On failure it records an `init_warning`
(`access_denied` / `no_device`) and continues without AMD CPU telemetry; the
Super I/O fan path loads independently of the AMD path. Live hardware access
requires administrator privileges (`RunLevel=Highest` on the scheduled tasks,
`Install-SVG-MB-ControlCommon.ps1`).

Runtime status and health output expose FEAT-0004's additive hardware-access
state as `hwaccess_state`, `hwaccess_read_state`, and
`hwaccess_write_state`, with separate detail strings for the AMD/SMN read path
and Super I/O write path. The signal is observational only: this repo still
does not install, start, restart, or repair PawnIO, and health exit-code
mapping is unchanged.

PawnIO loads bytecode modules and executes named functions in them (load IOCTL
`0xA084`, execute IOCTL `0xA104`) rather than the app issuing register or port
access directly. The app loads two modules:

- `AMDFamily17.bin` — AMD CPU registers. CPU temperature is read from the
  **System Management Network (SMN)**, not from MSR: Tctl/Tdie at SMN
  `0x00059800` and per-CCD Tdie at Zen2 `0x00059954` / Zen4 `0x00059B08`,
  serialized by the `Global\Access_PCI` mutex (`src\hardware\amd_reader.cpp`).
  FEAT-0006 also uses the module's `ioctl_read_msr` path when the default-off
  environment gates are explicitly enabled: RAPL package energy reads only
  `0xC0010299` / `0xC001029B`, and APERF/MPERF cycle evidence reads only the AMD
  read-only aliases `0xC00000E7` / `0xC00000E8` under a transient affinity pin.
  The app does not call `ioctl_write_msr`.
- `LpcIO.bin` — NCT6701D Super I/O over LPC. Fan reads, PWM duty writes, restore,
  voltage reads, temperature reads, and raw register access run as kernel-side
  port I/O through PawnIO (`ioctl_pio_inb` / `ioctl_pio_outb`,
  `ioctl_superio_inb` / `ioctl_superio_outb`); the app does not issue user-mode
  `in`/`out` instructions. Index/data ports `0x2E/0x2F` or `0x4E/0x4F` are
  probed, and access is serialized by the `Global\Access_ISABUS.HTP.Method` ISA
  mutex (`third_party\SVG-MB-SIO\src\fan_sio.cpp`).

So hardware access is direct in the sense of kernel-level register/port I/O, but
every access is marshalled through the PawnIO driver rather than a private ring-0
path in this app.

### VBS / HVCI (not addressed in this repo)

This repo contains no handling, configuration, or test coverage for
Virtualization-Based Security (VBS), HVCI / Memory Integrity, test signing, or
the Microsoft vulnerable-driver blocklist. (The only `VBS` string in the repo
refers to a deprecated `.vbs` watchdog script, not Virtualization-Based
Security: `docs\archive\implemented-plans\SCRIPT_STACK_REVIEW.md`.) The only stated requirement for live
hardware access is administrator privileges (above). If a security feature
blocked the driver, it would surface as an `access_denied` / `no_device`
`init_warning` from the AMD reader or SIO writer (`src\hardware\amd_reader.cpp`,
`src\hardware\sio_fan_writer.cpp`); there is no documented remediation path.

The following is a recommendation, not current repo behavior, and should be
confirmed against the installed driver:

- VBS + HVCI (Memory Integrity) ON does not, by itself, block this access model.
  HVCI requires a signed, HVCI-compatible driver; PawnIO is designed to meet
  that by running sandboxed bytecode modules instead of exposing arbitrary MSR
  or port I/O. The feature that retires older tools is the vulnerable-driver
  blocklist (which blocks WinRing0 / inpoutx64-style drivers); PawnIO is the
  replacement chosen to survive it.
- The signing status of the installed `PawnIO.sys`, its install method, and the
  current blocklist state are out of scope for this repo (the driver `.sys` is
  not vendored here). Verify empirically: enable Core Isolation → Memory
  Integrity, reboot, run `svg-mb-control.exe --diagnose-amd` and
  `--diagnose-gpu`, and confirm the reported `init_warning` is clean.

## Runtime Control Math (brief)

This is a one-screen orientation only. `docs\CONTROL_PIPELINE_MATH.md` is the
normative numerical reference (every operator with its source line), and
`docs\CONTROL_LOOP.md` covers the lifecycle (start, stop, supervision,
persistence).

In `control-loop` mode, `svg-mb-control.exe` runs a fixed-cadence tick. Each
tick (`tick_runner.cpp:RunControlTick`) turns telemetry into a per-channel fan
duty setpoint through this pipeline:

1. **Temperature inputs.** Read CPU (AMD Tctl/Tdie by configured label) and a
   GPU control envelope (`max` of core, memory-junction, and positive hotspot).
   Each channel selects a primary curve temperature from its `temp_blend`
   (`cpu_only`, `gpu_only`, `max_cpu_gpu`, or `max_cpu_gpu_source_aware` with a
   CPU-hot guard).
2. **Curve lookup.** The primary temperature maps through a piecewise curve
   (linear or quintic smootherstep) to a raw demand `r`, floored at
   `min_duty_pct` and clipped to `[0, 100]`. An optional `cpu_override_curve`
   takes the max with a CPU-driven demand.
3. **Demand smoothing.** `r` is smoothed into `s` by a per-direction EMA, with a
   per-minute "decay latch" that limits how fast duty falls while near the
   configured high band.
4. **Pressure boosts.** Four additive integrators accrue on top of `s`:
   thermal-pressure, mid-band, and GPU-airflow (seconds-scale, shared
   `boost_stage.cpp` integrator) plus CPU low-soak (minutes-scale, hysteretic
   release). Each is gated by a smootherstep band, clamped to a max, and has
   anti-windup at its ceiling.
5. **Low-band stage.** A slower, second-priority integrator
   (`low_band_integrator.cpp`) builds a global "debt" from sustained
   mid/upper-low temperatures, yields whenever any primary boost is active, and
   contributes a rate-limited per-channel boost capped by
   `low_band_residual_cap_pct`.
6. **Setpoint and rate limit.** The desired setpoint is
   `clip(s + all boosts, 0, 100)`, then rate-limited per channel against the
   time since the last successful write.
7. **Output gates.** The setpoint is written only if it passes deadband,
   authority-reassert, cooldown, baseline-captured, write-policy, and
   circuit-breaker gates; repeated write failures (5 consecutive) open the
   per-channel breaker.
8. **Adaptive cadence.** The next tick interval tightens toward
   `poll_tick_floor_ms` on fast temperature slew and relaxes back toward
   `poll_tick_ms`. With the shipped profile (`floor == poll`), cadence is a
   no-op and the interval is constant.

Two safety paths sit on top: **sensor-safe mode** drives a channel to 100% after
3 consecutive undefined primary-temperature ticks, and the setpoint is always
bounded to `[0, 100]` regardless of curve or integrator misconfiguration.
