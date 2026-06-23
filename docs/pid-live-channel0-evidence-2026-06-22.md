# FEAT-0003 Channel-0 Live PID Gate Evidence - 2026-06-22

Status: **current evidence record** for FEAT-0003 `REQ-PROFILE-07` M.

This note records a short, operator-authorized live PID opt-in for channel 0
only. It proves the live gate mechanics for the single channel identified by
`docs/pid-shadow-characterization-2026-06-21.md`; it does not authorize
all-channel PID and does not change the shipped/default `curve_overlay` profile.

## Package

The evidence binary is a clean-tree build of committed commit `913dda3`, so the
package provenance is reproducible from source (no working-tree/dirty build).

- Packaged executable: `release\svg-mb-control.exe`
- Version: `svg-mb-control 0.1.0 (913dda3e5e3d)`
- Release archive: `release\archive\svg-mb-control-20260622-1053.zip`
- `release\build-info.json`: `sourceCommit`
  `913dda3e5e3d99581e5b5fef8bceaa1e593e301c`, `sha256`
  `69B18EA1527A5ACB32CCCAC2202A369AC1B7B46DE1E65FC5A3FED9EF1AB40186`,
  `testsRun=true`, `testsPassed=true`, `builtUtc=2026-06-22T08:53:21Z`.
- Pre-live validation: `.\build-release.ps1 -KeepBuildDir` rebuilt and published
  the package from a clean working tree; the full hermetic test suite (CTest +
  pytest) ran and passed as part of the build, and the stamped `sourceCommit`
  matches the committed source.

## Config

Temporary live config:
`D:\tmp\svg-mb-control-pid-channel0-live-20260622-reevidence.json`.

It is `release\control.json` with the runtime paths absolutized to the packaged
`release\runtime` home (`runtime_home_path`, `runtime_policy_path`,
`snapshot_path`) and only channel 0 changed:

- `controller: "pid"`
- `pid.target_c: 68.0`
- `pid.kp: 0.3`
- `pid.ki: 0.01`
- `pid.kd: 0.0`
- `pid.feedforward: "curve"`
- `pid.allow_live: true`
- `pid.characterization_artifact`:
  `D:\Development\Thermals\SVG-MB\SVG-MB-Control\release\docs\pid-shadow-characterization-2026-06-21.md`
- Shared safety slew cap retained: `max_setpoint_step_pct: 0.6`
- Channel-0 floor retained: `min_duty_pct: 15.5`

All other channels stayed on `curve_overlay`. The config was preflighted with
`release\svg-mb-control.exe --show-config --json --config <pid-config>` and the
characterization artifact path existed in the release package.

## Run

The run used explicit stop/start boundaries (not `--restart`) to avoid
overlapping workers, and the `SVG-MB Control Watchdog` scheduled task was
disabled for the duration and re-enabled afterward so a 60 s watchdog tick could
not relaunch the default config into the stop/start gap. All runtime files
(status, CSV, event log) are under the packaged `release\runtime` home.

| Time | Step | Evidence |
|---|---|---|
| `2026-06-22T11:02:58` | Baseline default controller | Healthy; channel 0 `controller_kind=curve_overlay`, status `running`. |
| `2026-06-22T11:02:58` | Stop default package | `--stop --config release\control.json` returned `stopped`; `--health` reported `health_state=stopped` (exit 2). |
| `2026-06-22T11:03:02` | PID law applied live | Event `control_loop.profile_applied` channel 0 `detail=law=pid live (allow_live evidenced + slew cap)`, then `control_loop.write_applied source=pid`. |
| `2026-06-22T11:03:03` | Start temporary PID config | `--start --config <pid-config>`; supervisor `31824`; worker `55280`; channel 0 `controller_kind=pid`, `last_response_source=pid`. |
| `2026-06-22T11:04:04` | End of 60 s PID hold | Healthy; channel 0 `controller_kind=pid`, `last_response_source=pid`, observed GPU temp `46.0 C`, issued setpoint `15.5%` (channel-0 floor), raw PID setpoint `8.9%`, `pid_error_c=-22.0`, `pid_p_term=-6.6`, `pid_i_term=0.0`, `pid_d_term=0.0`, `total_writes` 5 -> 61 (56 live writes), no breaker. |
| `2026-06-22T11:04:04` | Stop temporary PID config | `--stop --config <pid-config>` returned `stopped`. |
| `2026-06-22T11:04:11` | Roll back to packaged default | `--start --config release\control.json`; supervisor `67476`; worker `43032`; channel 0 `controller_kind=curve_overlay`. |
| `2026-06-22T11:04:12` | Final live-state check | Healthy on the default release config; watchdog re-enabled (`Ready`). |

During the hold the raw PID request (`8.9%`) stayed below the channel-0 floor;
the issued setpoint slew-limited down from the baseline value toward the `15.5%`
floor (0.6%/tick cap) and held there.

Compact run summary:
`D:\tmp\svg-mb-control-pid-reevidence-20260622-summary.json`.

Runtime evidence paths:

- PID window CSV:
  `release\runtime\logs\archive\svg_mb_control_control-loop_20260622_110302.csv`
- Event log:
  `release\runtime\logs\svg_mb_control_events.jsonl`

## Result

`REQ-PROFILE-07` M passes for the channel-0-only live gate:

- PID stayed shadow/dry-run by default and became live only under explicit
  `pid.allow_live`, an existing characterization artifact, and a positive
  non-NaN slew cap.
- The live channel used the shared clamp and slew conditioning; during the hold
  the raw PID request was below the safety floor and the issued setpoint
  slew-limited to the channel-0 floor (`15.5%`).
- Health stayed `healthy`, no circuit breaker opened, and channel 0 remained
  `controller_kind=pid` for the proof window.
- The temporary config was stopped, restored, and the packaged default config
  was relaunched; final status was healthy on `curve_overlay`.

Scope: this run validates the live gate path only (shadow-by-default, the
`allow_live` + characterization-artifact + slew-cap opt-in, shared clamp/slew
conditioning, and clean rollback). Because the GPU was idle (`46.0 C` vs target
`68 C`), the raw PID request stayed below the floor, so the run does not
demonstrate above-floor PID modulation or tuning efficacy; that remains out of
scope for `REQ-PROFILE-07` M and gated behind a future characterization record.

The all-channel PID result from
`docs/pid-shadow-characterization-2026-06-21.md` remains rejected. Future PID
tuning beyond this channel-0 gate proof requires a new characterization record
and explicit live-runtime authorization.

## Notes

The package is a clean-tree rebuild (commit `913dda3`) of the reviewed change
set, so the live-evidence binary is reproducible from a committed `sourceCommit`
rather than a working-tree (dirty) build. An earlier shipped-package stamp on
the pre-commit working tree was the reason this clean rebuild was performed.

A first attempt with the temporary config under `D:\tmp` aborted at startup
because the copied config kept relative runtime paths (`runtime_home_path`,
`runtime_policy_path`) that resolved under `D:\tmp`; the controller rolled back
to `release\control.json` automatically with no fan-control effect. The accepted
evidence above used a config with those paths absolutized to the packaged
`release\runtime` home.
