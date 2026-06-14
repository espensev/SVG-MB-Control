# Discovery - Watchdog PowerShell Window

**Goal:** Identify which SVG-MB Control process launches a PowerShell window and how it happens.
**Date:** 2026-05-19
**Status:** complete
**Recommended next:** implemented with native no-console watchdog helper; no larger planning needed.

---

## Questions

1. What are the exact actions configured for `SVG-MB Control` and `SVG-MB Control Watchdog`?
2. Does either task directly start `powershell.exe`?
3. What does the watchdog do when it runs?
4. Is the visible PowerShell window caused by the watchdog task, the main control task, or supervisor child process behavior?

---

## Findings

### Q1: What are the exact actions configured?

**Answer:** The main task starts the native control executable directly. The watchdog task starts Windows PowerShell and runs the watchdog script with `-Run`.

**Evidence:**
- Scheduled task query, 2026-05-19 00:10 local:
  - Main task action: `D:\Development\Thermals\SVG-MB\SVG-MB-Control\release\svg-mb-control.exe --start --config "...release\control.json"`
  - Watchdog action: `C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File "...release\Install-SVG-MB-ControlWatchdogScheduledTask.ps1" -Run`
- `Install-SVG-MB-ControlScheduledTask.ps1:163-167` builds the main task action against `$exePath`.
- `Install-SVG-MB-ControlWatchdogScheduledTask.ps1:227-236` builds the watchdog task action against `powershell.exe`.

**Implications:**
- The watchdog task is the only scheduled task in this pair that directly launches PowerShell.

### Q2: Does either task directly start `powershell.exe`?

**Answer:** Yes. Only `SVG-MB Control Watchdog` directly starts `powershell.exe`.

**Evidence:**
- `Install-SVG-MB-ControlWatchdogScheduledTask.ps1:227-236` resolves `powershell.exe` and passes it to `New-ScheduledTaskAction`.
- Exported task XML confirms `<Command>C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe</Command>`.
- Manual poll after `Start-ScheduledTask` caught:
  - Parent: `svchost.exe -k netsvcs -p -s Schedule`
  - Child: `"C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "...Install-SVG-MB-ControlWatchdogScheduledTask.ps1" -Run`

**Implications:**
- A visible PowerShell window is consistent with the watchdog task action and its interactive principal.

### Q3: What does the watchdog do when it runs?

**Answer:** It runs `svg-mb-control.exe --health --json --config ...`, parses the result, exits successfully for healthy/degraded, restarts the control task only when health exits stale, and does not auto-restart on failed health.

**Evidence:**
- `Install-SVG-MB-ControlWatchdogScheduledTask.ps1:117-120` invokes `--health --json --config`.
- `Install-SVG-MB-ControlWatchdogScheduledTask.ps1:148-150` returns `0` for healthy/degraded.
- `Install-SVG-MB-ControlWatchdogScheduledTask.ps1:152-160` invokes `--restart --config` for stale.
- `Install-SVG-MB-ControlWatchdogScheduledTask.ps1:163-164` warns and returns failed health without restart.

**Implications:**
- The repeated PowerShell launch is not accidental; it is how the watchdog is currently implemented.

### Q4: Is the visible window caused by watchdog, main task, or supervisor?

**Answer:** The PowerShell window is caused by the watchdog scheduled task. The main task does not launch PowerShell, and the native supervisor launches managed runtime children with `CREATE_NO_WINDOW`.

**Evidence:**
- `Install-SVG-MB-ControlScheduledTask.ps1:163-167` uses the native executable directly.
- `src/control_supervisor.cpp:360-368` starts managed native child processes with `CREATE_NO_WINDOW`.
- Watchdog task principal is interactive/highest privilege: `Install-SVG-MB-ControlWatchdogScheduledTask.ps1:243-246`.
- Watchdog trigger repeats every minute: `Install-SVG-MB-ControlWatchdogScheduledTask.ps1:237-242`.

**Implications:**
- Fixing the visible PowerShell window should target the watchdog scheduled-task action/registration path.

---

## Cross-Cutting Analysis

### Constraints

- The watchdog currently depends on PowerShell script logic for health parsing and stale restart behavior.
- The task is registered as `LogonType Interactive` and `RunLevel Highest`, which allows user-session UI.
- The task repeats every minute, so even a short window flash can recur continuously.

### Risks

| Risk | Likelihood | Impact | Notes |
|------|------------|--------|-------|
| Window flash every minute | High | Medium | Current action directly launches interactive `powershell.exe`. |
| Hiding PowerShell masks useful debug output | Medium | Low | The script currently writes health/reason to host output. |
| Replacing the script with native logic changes watchdog semantics | Medium | Medium | Native equivalent must preserve stale-only restart behavior. |

### Open Questions

- None for root cause. Implementation choice remains: hide the existing PowerShell watchdog or replace it with native watchdog logic.

---

## Recommendation

This is small enough to fix directly. The lowest-risk fix is to add hidden/noninteractive launch flags to the watchdog scheduled task action and reinstall/update the task. The cleaner long-term fix is to move the watchdog `--health`/stale restart flow into `svg-mb-control.exe` and register the watchdog task against the native exe, eliminating PowerShell from the recurring task entirely.

---

## Fix Options

### Option 1: Hide the existing PowerShell watchdog

Change the watchdog scheduled task action to add `-WindowStyle Hidden` and
`-NonInteractive`, and register the task as hidden.

**Pros:**
- Smallest change.
- Preserves the existing script and semantics.
- Can be shipped quickly.

**Cons:**
- Still launches `powershell.exe` once per minute.
- `-WindowStyle Hidden` is not as strong as removing the console path entirely;
  Windows may still create the process before PowerShell parses the flag.

**Use when:** immediate relief is needed and a small residual flash risk is acceptable.

### Option 2: Run the watchdog task non-interactively

Register the watchdog with a non-interactive principal so it cannot show UI.

**Pros:**
- Prevents user-session windows.
- Keeps the script mostly intact.

**Cons:**
- Risky for restart semantics: the watchdog currently calls
  `svg-mb-control.exe --restart`, which would launch the controller from the
  watchdog task context. If that context changes, the restarted controller may
  not match the normal interactive/highest launch path.

**Use when:** the restart path is changed to stop the controller and then start
the normal `SVG-MB Control` task, rather than launching the runtime directly.

### Option 3: Add a no-console native watchdog launcher

Add a small Windows-subsystem helper such as `svg-mb-control-watchdog.exe`.
The scheduled task runs that helper directly. The helper launches
`svg-mb-control.exe --health --json` and, only for stale/stopped exit code `2`,
launches `svg-mb-control.exe --restart`, using `CREATE_NO_WINDOW` with stdout
and stderr captured to logs.

**Pros:**
- Strongest no-window behavior while preserving the current restart semantics.
- Removes PowerShell from the recurring task.
- Keeps the normal main controller task unchanged.
- Can reuse health exit codes, so it does not need to parse JSON.

**Cons:**
- Adds a second executable target.
- Needs installer/script/docs/test updates.

**Use when:** the watchdog must be reliable and must never show a PowerShell or
console window.

### Option 4: Move stale detection into the supervisor

Teach the already-running supervisor to restart stale workers internally and
disable the scheduled watchdog.

**Pros:**
- No recurring scheduled task, so no recurring launch window.
- Keeps all runtime behavior in the native process tree.

**Cons:**
- Loses external recovery if the supervisor itself exits or hangs.
- Changes the watchdog model from external health monitor to internal worker
  monitor.

**Use when:** internal recovery is sufficient and external process recovery is no longer required.

### Recommended Path

Implement Option 3 as the durable fix. Option 1 can be used as a temporary
mitigation, but it does not fully satisfy a strict "must not show a window"
requirement.

### Implementation Result

Option 3 is now implemented:

- `src/watchdog_main.cpp` builds `svg-mb-control-watchdog.exe` as a
  Windows-subsystem helper.
- The helper launches `svg-mb-control.exe --health --json` and, only for
  stale/stopped exit code `2`, `svg-mb-control.exe --restart` with
  `CREATE_NO_WINDOW`.
- `Install-SVG-MB-ControlWatchdogScheduledTask.ps1` now registers the recurring
  watchdog task against `svg-mb-control-watchdog.exe`, not `powershell.exe`.
- Runtime logs are written under `runtime\logs\svg-mb-control-watchdog*.log`.
