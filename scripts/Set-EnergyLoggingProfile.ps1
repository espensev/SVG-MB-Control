# Operator profile switch for STANDARD control-loop CPU package-power logging
# (FEAT-0020 / REQ-PWRLOG-06). Flips the persistent worker environment so the
# shipped control loop records the FEAT-0006 RAPL package-energy columns, and
# makes that profile survive the boot/logon 'SVG-MB Energy Safety Revert' task
# (which otherwise forces energy OFF within ~120 s of every restart).
#
#   -Enable  : disable the Safety Revert task, set User
#              SVG_MB_CONTROL_RAPL_ENERGY_MODE='enabled' (CPU cycles left
#              'disabled'), then restart the worker tree so it re-reads the env.
#              The live cpu_pkg_energy_acquisition marker becomes 'quarantine' on
#              healthy hardware; it is NEVER auto-promoted to 'validated'.
#   -Disable : re-enable the Safety Revert task, set the var back to 'disabled',
#              then restart the worker tree (restores the FEAT-0006 safety net).
#
# GOVERNANCE: -Enable intentionally suspends FEAT-0006's "never leave energy
# enabled after a restart until the gate passes" guarantee for the standard loop.
# That inversion is the recorded decision in
# docs/power-logging-flip-plan-2026-06-18.md (D-PWRLOG-1). It does not change the
# acquisition marker semantics (the worker still tops out at 'quarantine').
#
# -DryRun prints the planned actions and changes nothing (no admin required); use
# it to review or test the workflow without touching the live runtime. The live
# flip itself requires explicit live-runtime authorization (AGENTS.md Live
# Runtime Safety).
[CmdletBinding()]
param(
    [switch]$Enable,
    [switch]$Disable,
    [switch]$DryRun,
    [switch]$NoElevate,
    [switch]$NoRestart
)

$ErrorActionPreference = 'Stop'

if ($Enable -eq $Disable) {
    throw 'Specify exactly one of -Enable or -Disable.'
}
$Mode = if ($Enable) { 'Enable' } else { 'Disable' }
$TargetEnergy = if ($Enable) { 'enabled' } else { 'disabled' }

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ReleaseRoot = Join-Path $RepoRoot 'release'
$TaskPath = '\SVG-MB Control\'
$TaskName = 'SVG-MB Control'
$WatchdogName = 'SVG-MB Control Watchdog'
$SafetyRevertName = 'SVG-MB Energy Safety Revert'
$EnergyVar = 'SVG_MB_CONTROL_RAPL_ENERGY_MODE'
$CyclesVar = 'SVG_MB_CONTROL_CPU_CYCLES_MODE'

function Test-Admin {
    $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object System.Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole(
        [System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Scheduled-task control and the worker restart need admin. -DryRun is read-only.
if (-not $DryRun -and -not $NoElevate -and -not (Test-Admin)) {
    Write-Host 'Re-launching elevated (scheduled-task control needs admin)...' `
        -ForegroundColor Yellow
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        $PSCommandPath, '-NoElevate', "-$Mode")
    if ($NoRestart) { $argList += '-NoRestart' }
    Start-Process -FilePath (Get-Process -Id $PID).Path `
        -ArgumentList $argList -Verb RunAs
    return
}

# Run an action, or just describe it under -DryRun (which writes nothing).
function Invoke-OrEcho {
    param([Parameter(Mandatory)][string]$Description,
          [Parameter(Mandatory)][scriptblock]$Action)
    if ($DryRun) {
        Write-Host "  [dry-run] $Description" -ForegroundColor DarkGray
    } else {
        Write-Host "  $Description" -ForegroundColor Cyan
        & $Action
    }
}

# Restart the worker tree so it re-reads the persistent env (the var is frozen at
# process creation). Mirrors Capture-EnergySession.ps1: stop the watchdog first so
# it cannot respawn the worker mid-swap, stop + kill the release\ worker
# processes, relaunch the worker, then restore the watchdog. Best-effort on the
# scheduled-task calls (a missing/idle watchdog is not fatal).
function Restart-WorkerTree {
    param([string]$Reason)
    Write-Host "  restart worker tree ($Reason)" -ForegroundColor Cyan
    try {
        Stop-ScheduledTask -TaskPath $TaskPath -TaskName $WatchdogName `
            -ErrorAction Stop
    } catch { Write-Warning "Stop watchdog: $($_.Exception.Message)" }
    try {
        Stop-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName `
            -ErrorAction Stop
    } catch { Write-Warning "Stop worker: $($_.Exception.Message)" }
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        $procs = Get-CimInstance Win32_Process `
            -Filter "Name='svg-mb-control.exe'" -ErrorAction SilentlyContinue |
            Where-Object {
                $_.CommandLine -and $_.CommandLine -like "*$ReleaseRoot*"
            }
        if (-not $procs) { break }
        foreach ($p in $procs) {
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        }
        Start-Sleep -Milliseconds 500
    }
    Start-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName
    try {
        Start-ScheduledTask -TaskPath $TaskPath -TaskName $WatchdogName `
            -ErrorAction Stop
    } catch { Write-Warning "Start watchdog: $($_.Exception.Message)" }
}

Write-Host ("SVG-MB energy-logging profile: $Mode " +
    "(energy -> '$TargetEnergy', cycles -> 'disabled')") -ForegroundColor Green
if ($Enable) {
    Write-Host ("NOTE: -Enable suspends the FEAT-0006 boot-time energy-OFF " +
        "guarantee for the standard loop (D-PWRLOG-1). Use -Disable to " +
        "restore it.") -ForegroundColor Yellow
}

# 1. Safety Revert task: disabled while energy logging is enabled (so the boot/
#    logon revert cannot tear the profile down), re-enabled on -Disable. Best
#    effort: if the task was never registered there is nothing to tear the
#    profile down, so a missing task is only a warning.
if ($Enable) {
    Invoke-OrEcho "Disable-ScheduledTask '$TaskPath$SafetyRevertName'" {
        try {
            Disable-ScheduledTask -TaskPath $TaskPath `
                -TaskName $SafetyRevertName -ErrorAction Stop | Out-Null
        } catch {
            Write-Warning ("Disable-ScheduledTask '$SafetyRevertName': " +
                "$($_.Exception.Message)")
        }
    }
} else {
    Invoke-OrEcho "Enable-ScheduledTask '$TaskPath$SafetyRevertName'" {
        try {
            Enable-ScheduledTask -TaskPath $TaskPath `
                -TaskName $SafetyRevertName -ErrorAction Stop | Out-Null
        } catch {
            Write-Warning ("Enable-ScheduledTask '$SafetyRevertName': " +
                "$($_.Exception.Message)")
        }
    }
}

# 2. Persistent worker env (User scope; the worker reads it at process creation).
Invoke-OrEcho "Set User $EnergyVar = '$TargetEnergy'" {
    [Environment]::SetEnvironmentVariable($EnergyVar, $TargetEnergy, 'User')
}
# CPU cycles stay disabled in both directions; FEAT-0020 keeps APERF/MPERF off
# unless a run explicitly needs effective-frequency context.
Invoke-OrEcho "Set User $CyclesVar = 'disabled'" {
    [Environment]::SetEnvironmentVariable($CyclesVar, 'disabled', 'User')
}

# 3. Restart the worker so it re-reads the env. Skipped with -NoRestart (e.g.
#    when staging before a separate restart) or -DryRun.
if ($NoRestart) {
    Write-Host '  (skipping worker restart; -NoRestart)' -ForegroundColor DarkGray
} elseif ($DryRun) {
    Write-Host "  [dry-run] restart worker tree so it re-reads $EnergyVar" `
        -ForegroundColor DarkGray
} else {
    Restart-WorkerTree -Reason "energy-logging $Mode"
}

Write-Host ("Done. After the worker restarts, verify the control-loop CSV " +
    "'$EnergyVar' marker (expect 'quarantine' when enabled).") `
    -ForegroundColor Green
