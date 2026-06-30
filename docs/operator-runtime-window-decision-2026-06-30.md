# Operator Runtime Window Decision - 2026-06-30

**Status:** Current
**Feature:** FEAT-0026 (`REQ-OPWINDOW-*`)

## Decision

Intentional controller off windows are handled by a repo-local PowerShell
operator helper, `Set-SVG-MB-ControlRuntimeWindow.ps1`, rather than by adding a
new runtime daemon or an in-process pause state.

The helper uses existing supported surfaces:

- the packaged `svg-mb-control.exe --stop`, `--start`, and `--restart` paths;
- the existing `SVG-MB Control` and `SVG-MB Control Watchdog` scheduled-task
  names from `Install-SVG-MB-ControlCommon.ps1`;
- a one-shot resume scheduled task for bounded windows;
- optional foreground `--mode evidence-log` for read-only evidence during a
  bounded off window;
- `-Status -Json` as the machine-readable process-boundary state surface for
  future external coordinators such as SQ-control.

## Rationale

The existing runtime already has cooperative stop/restart semantics and normal
shutdown restore. Adding a separate in-process "pause for N minutes" state would
duplicate lifecycle ownership and add another control path that could drift from
the tested `--stop`/`--restart` behavior.

Task Scheduler is already the repo's boot/logon recovery surface. A one-shot
resume task keeps a bounded off window durable across a reboot without requiring
a resident helper process.

Control-loop CSV logging necessarily stops when the controller is intentionally
stopped. When telemetry matters during that off window, the correct existing
logging plane is read-only `evidence-log`, which writes separate
`svg_mb_control_evidence.*` artifacts and does not write fan duty.

## Safety Rules

- The helper must not write fan duty directly.
- The helper must not reset circuit breakers.
- The helper must disable the watchdog before stopping Control for an off
  window so the watchdog does not undo the operator request.
- A resume must stop optional evidence-log first, then start Control, because
  both long-running modes observe the shared `stop.request.json`.
- `-DryRun` must remain available for review and tests without touching live
  scheduled tasks or runtime state.

## Consequences

- A bounded pause means "Control off until the resume time"; fans return to the
  normal BIOS/restore behavior from the existing shutdown path.
- Optional logging during the window is evidence logging, not control-loop CSV
  logging.
- The helper owns only its small `runtime\operator_windows\*.json` state file
  and one-shot scheduled tasks. Those are operator-helper artifacts, not
  Control-owned runtime schema.
- Future SQ-control coordination should call this packaged helper or the
  packaged executable as a process boundary; SVG-MB-Control remains standalone.
