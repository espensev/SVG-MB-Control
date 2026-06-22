# FEAT-0003 Channel-0 Live PID Gate Evidence - 2026-06-22

Status: **current evidence record** for FEAT-0003 `REQ-PROFILE-07` M.

This note records a short, operator-authorized live PID opt-in for channel 0
only. It proves the live gate mechanics for the single channel identified by
`docs/pid-shadow-characterization-2026-06-21.md`; it does not authorize
all-channel PID and does not change the shipped/default `curve_overlay` profile.

## Package

- Packaged executable: `release\svg-mb-control.exe`
- Version: `svg-mb-control 0.1.0 (3ce1c346bcf5)`
- Release archive: `release\archive\svg-mb-control-20260622-0745.zip`
- `release\build-info.json`: `sourceCommit`
  `3ce1c346bcf5cfa7a5a58797ab7d99864f2de080`, `testsRun=true`,
  `testsPassed=true`
- Pre-live validation: `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` passed and
  `.\build-release.ps1 -KeepBuildDir` rebuilt/published the package.

## Config

Temporary live config:
`D:\tmp\svg-mb-control-pid-channel0-live-20260622.json`.

Only channel 0 changed from the packaged release config:

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

All other channels stayed on `curve_overlay`. The config was preflighted with
`release\svg-mb-control.exe --show-config --json --config <pid-config>` and the
characterization artifact path existed in the release package.

## Run

The accepted run used explicit stop/start boundaries to avoid overlapping
workers during the temporary config switch.

| Time | Step | Evidence |
|---|---|---|
| `2026-06-22T07:51:21` | Baseline default controller | Healthy; worker `38052`; supervisor `24552`; channel 0 `controller_kind=curve_overlay`; CSV `svg_mb_control_control-loop_20260622_075023.csv`. |
| `2026-06-22T07:51:22` | Stop default package | `--stop --config release\control.json` returned success; health reported `stopped`, `last_successful_restore_time=2026-06-22T07:51:22`, `pending_write_count=0`. |
| `2026-06-22T07:51:24` | Start temporary PID config | `--start --config D:\tmp\svg-mb-control-pid-channel0-live-20260622.json`; supervisor `38756`; worker `41976`; channel 0 `controller_kind=pid`. |
| `2026-06-22T07:52:24` | End of 60 s PID hold | Healthy; channel 0 still `controller_kind=pid`, `last_response_source=pid`, observed GPU temp `46.0 C`, setpoint `15.5%`, raw PID setpoint `8.9%`, `pid_error_c=-22.0`, `pid_p_term=-6.6`, `pid_i_term=0.0`, `pid_d_term=0.0`, `total_writes=68`. |
| `2026-06-22T07:52:24` | Stop temporary PID config | `--stop --config <pid-config>` returned success; shutdown restore applied. |
| `2026-06-22T07:52:27` | Roll back to packaged default | `--start --config release\control.json`; healthy; worker `35080`; supervisor `40576`; channel 0 `controller_kind=curve_overlay`. |
| `2026-06-22T07:53:42` | Final live-state check | Running on default release config; channel 0 `curve_overlay`; CSV `svg_mb_control_control-loop_20260622_075225.csv`. |

Compact run summary:
`D:\tmp\svg-mb-control-pid-live-20260622-summary.json`.

Runtime evidence paths:

- PID window CSV:
  `release\runtime\logs\archive\svg_mb_control_control-loop_20260622_075122.csv`
- PID window manifest:
  `release\runtime\logs\archive\svg_mb_control_control-loop_20260622_075122.manifest.json`
- Event log:
  `release\runtime\logs\svg_mb_control_events.jsonl`

The event log includes `control_loop.profile_applied` for channel 0 with
`law=pid live (allow_live evidenced + slew cap)`, then shutdown restore events
and a normal default `control_loop.start` after rollback.

## Result

`REQ-PROFILE-07` M passes for the channel-0-only live gate:

- PID stayed shadow/dry-run by default and became live only under explicit
  `pid.allow_live`, an existing characterization artifact, and a positive
  non-NaN slew cap.
- The live channel used the shared clamp and slew conditioning; during the hold
  the raw PID request was below the safety floor and the issued setpoint stayed
  at the channel-0 floor (`15.5%`).
- Health stayed `healthy`, no circuit breaker opened, and channel 0 remained
  `controller_kind=pid` for the proof window.
- The temporary config was stopped, restored, and the packaged default config
  was relaunched; final status was healthy on `curve_overlay`.

Scope: this run validates the live gate path only (shadow-by-default, the
`allow_live` + characterization-artifact + slew-cap opt-in, shared
clamp/slew conditioning, and clean rollback). Because the GPU was idle
(`46.0 C` vs target `68 C`), the raw PID request stayed below the floor, so the
run does not demonstrate above-floor PID modulation or tuning efficacy; that
remains out of scope for `REQ-PROFILE-07` M and gated behind a future
characterization record.

The all-channel PID result from
`docs/pid-shadow-characterization-2026-06-21.md` remains rejected. Future PID
tuning beyond this channel-0 gate proof requires a new characterization record
and explicit live-runtime authorization.

## Notes

A first direct `--restart --config <pid-config>` attempt aborted before an
accepted PID evidence window was established because startup overlapped runtime
sidecar/singleton reconciliation. It rolled back to `release\control.json`. The
accepted evidence above is the later stop/start-bounded run.
