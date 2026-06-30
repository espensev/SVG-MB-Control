# Operator Runtime Windows

**Purpose:** run intentional SVG-MB-Control stop, pause, resume, and restart
windows from one packaged helper.

This workflow is for deliberate operator windows: maintenance, tests, or
thermal experiments where Control should be stopped for a known period or until
manual resume. It uses only the packaged Control executable and the repo-defined
scheduled-task names.

## Commands

Run these from the release directory after publishing, or from the repo root
while testing the helper:

```powershell
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Status
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Status -Json
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -For 30m
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -For 2h30m
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -Until "2026-06-30T23:30:00"
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -For 45m -EvidenceLog
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Resume
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Restart
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Stop
```

Add `-DryRun` before a real window to print the planned state write, task
changes, lifecycle command, evidence-log task, and resume task without changing
Task Scheduler or runtime files.

## Window Behavior

`-Pause -For <duration>` and `-Pause -Until <datetime>` perform the same core
sequence:

1. record helper-owned state under `runtime\operator_windows\active_window.json`;
2. stop and disable the watchdog task;
3. stop and disable the main `SVG-MB Control` task;
4. request the normal cooperative stop through `svg-mb-control.exe --stop`;
5. register a one-shot resume task for the requested end time.

`-Stop` uses the same stop path without registering a resume task unless `-For`
or `-Until` is also supplied. Use `-Resume` to start Control again and restore
the previously recorded task-enabled states when they are known.

`-Restart` delegates to the packaged `svg-mb-control.exe --restart --config`
path. It is not a timed window.

## Logging During A Window

Control-loop CSV logging stops when Control is intentionally stopped.
Use `-EvidenceLog` on a bounded window when telemetry still matters:

```powershell
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -For 45m -EvidenceLog
```

The helper starts read-only `svg-mb-control.exe --mode evidence-log --config
<control.json>` in a separate one-shot scheduled task. Evidence logging writes
separate `svg_mb_control_evidence.*` CSV, event, and manifest files. On resume,
the helper requests evidence-log stop first, waits for the matching process to
exit, then starts Control.

## Coordinator Contract

Future external coordinators, including SQ-control, should treat this repo as a
process boundary. They should call the packaged helper or packaged executable
instead of importing code from this repo.

Machine-readable helper state is available through:

```powershell
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Status -Json
```

The JSON status has `schema_version = 1` and includes:

- `coordinator_contract = "process-boundary"`;
- `sibling_repo_dependency = false`;
- resolved `script_path`, `exe_path`, `config_path`, `runtime_home`, and
  `state_path`;
- `active_window`, which is the helper-owned active window state or `null`;
- task summaries for `control`, `watchdog`, `resume`, and `evidence`.

Coordinators that need live controller health should still call the packaged
Control status/health CLI directly after reading the helper JSON.

## Safety Boundaries

The helper does not write fan duty directly, does not reset breakers, and does
not change control math, runtime schemas, curves, cadence, or shipped config.
It only orchestrates existing lifecycle commands and scheduled tasks when
explicitly invoked.

Live task mutation requires elevation. Non-elevated mutating runs relaunch the
same helper elevated unless `-NoElevate` is supplied.

## First Live Use

Before relying on an unattended timed window, run a supervised dry-run and a
short live window at a low-risk time:

```powershell
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -For 5m -EvidenceLog -DryRun
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Status -Json
release\svg-mb-control.exe --status --config .\release\control.json
release\svg-mb-control.exe --health --json --config .\release\control.json
```

If a real stop attempt fails after tasks have been disabled, inspect `-Status`
and run `-Resume` to restore the recorded task-enabled state before trying a new
window. Keep the first live test supervised because the dry-run and packaging
checks verify command shape and packaging, not Windows Task Scheduler execution
on the installed machine.
