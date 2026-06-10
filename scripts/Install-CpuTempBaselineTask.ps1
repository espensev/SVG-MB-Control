#requires -Version 5.1
<#
.SYNOPSIS
    Install/manage a Windows scheduled task that records a long-term CPU
    temperature-by-load baseline using Compare-CpuTemps.ps1.

.DESCRIPTION
    Registers a scheduled task that runs scripts\Compare-CpuTemps.ps1 every
    -IntervalMinutes, each run summarizing the last -WindowMinutes of the live
    control-loop CSV into one ledger data point under a fixed -Label. Over days
    this builds a time series you can use as a pre-overclock / pre-undervolt
    baseline and compare against later settings (the ledger's `comparison.md`
    aggregates per setting).

    The task is read-only: it runs at RunLevel Limited (no elevation), reads the
    CSV the controller already writes, and appends only to the experiments
    ledger. It does not start/stop the controller or write fan duty.

    AMBIENT: the harness cannot read room temperature, so the task records a fixed
    -AmbientC for every run. Pass your typical room temperature. If the room
    temperature changes materially during the baseline, edit `ambient_c` in the
    ledger or re-install with a new value.

.PARAMETER AmbientC
    Required for -Install. Fixed room/ambient temperature (C) recorded on every run.

.PARAMETER Label
    Ledger label for this baseline. Default 'stock-preoc'.

.PARAMETER IntervalMinutes
    How often the task records a data point. Default 15.

.PARAMETER WindowMinutes
    Recent window each run summarizes. Default 30.

.PARAMETER DwellSeconds
    Dwell-gate seconds passed to the harness. Default 45.

.PARAMETER Notes
    Optional notes recorded on every run.

.PARAMETER Install / Start / Stop / Status / Remove
    Action to take. Default (no switch) is -Install. -Install also runs the task
    once immediately unless -NoStart is given.

.EXAMPLE
    .\scripts\Install-CpuTempBaselineTask.ps1 -AmbientC 22

.EXAMPLE
    .\scripts\Install-CpuTempBaselineTask.ps1 -Status
    .\scripts\Install-CpuTempBaselineTask.ps1 -Remove

.NOTES
    Reversible: -Remove unregisters the task. If registration is denied, run from
    an elevated PowerShell (creating tasks in the shared task folder can require
    admin on some systems).
#>
[CmdletBinding()]
param(
    [double]$AmbientC = [double]::NaN,
    [string]$Label = 'stock-preoc',
    [int]$IntervalMinutes = 15,
    [int]$WindowMinutes = 30,
    [int]$DwellSeconds = 45,
    [string]$Notes = '',
    [string]$TaskName = 'SVG-MB CPU Temp Baseline',
    [string]$TaskPath = '\SVG-MB Control\',
    [switch]$Install,
    [switch]$Start,
    [switch]$Stop,
    [switch]$Status,
    [switch]$Remove,
    [switch]$NoStart
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Invariant = [System.Globalization.CultureInfo]::InvariantCulture

$repoRoot = Split-Path -Parent $PSScriptRoot
$harness  = Join-Path $PSScriptRoot 'Compare-CpuTemps.ps1'
$ledger   = Join-Path $repoRoot 'release\runtime\experiments\cpu-temp-comparison\ledger.csv'

if (-not $TaskPath.EndsWith('\')) { $TaskPath = "$TaskPath\" }

function Resolve-PsHost {
    $pwsh = Get-Command pwsh.exe -ErrorAction SilentlyContinue
    if ($pwsh) { return $pwsh.Source }
    $ps = Get-Command powershell.exe -ErrorAction SilentlyContinue
    if ($ps) { return $ps.Source }
    throw 'Neither pwsh.exe nor powershell.exe was found on PATH.'
}

function Get-Task {
    try { Get-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath -ErrorAction Stop }
    catch { $null }
}

function Show-Status {
    $task = Get-Task
    if (-not $task) {
        Write-Host "Scheduled task: not installed ($TaskPath$TaskName)"
    } else {
        $info = Get-ScheduledTaskInfo -TaskName $TaskName -TaskPath $TaskPath -ErrorAction SilentlyContinue
        Write-Host "Scheduled task: $TaskPath$TaskName"
        Write-Host "  state      : $($task.State)"
        if ($info) {
            Write-Host "  last_run   : $($info.LastRunTime)"
            Write-Host "  last_result: $($info.LastTaskResult)"
            Write-Host "  next_run   : $($info.NextRunTime)"
        }
    }
    if (Test-Path -LiteralPath $ledger) {
        $rows = @(Import-Csv -LiteralPath $ledger)
        $caps = @($rows | ForEach-Object { $_.run_id } | Sort-Object -Unique).Count
        Write-Host "  ledger     : $ledger"
        Write-Host "  captures   : $caps"
    } else {
        Write-Host "  ledger     : (none yet) $ledger"
    }
}

# Default action.
if (-not ($Install -or $Start -or $Stop -or $Status -or $Remove)) { $Install = $true }
if ($Remove -and ($Install -or $Start -or $Stop)) {
    throw '-Remove cannot be combined with -Install/-Start/-Stop.'
}

if (-not (Test-Path -LiteralPath $harness -PathType Leaf)) {
    throw "Harness not found next to this installer: $harness"
}

if ($Remove) {
    if (Get-Task) {
        Unregister-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath -Confirm:$false
        Write-Host "Removed scheduled task: $TaskPath$TaskName"
    } else {
        Write-Host "Scheduled task already absent: $TaskPath$TaskName"
    }
    return
}

if ($Status) { Show-Status; return }

if ($Stop) {
    if (Get-Task) {
        Stop-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath -ErrorAction SilentlyContinue
        Write-Host "Stopped any running instance of: $TaskPath$TaskName (still registered)"
    } else {
        Write-Host "Scheduled task not installed: $TaskPath$TaskName"
    }
    return
}

if ($Install) {
    if ([double]::IsNaN($AmbientC)) {
        throw 'Provide -AmbientC <room temperature in C>. The harness cannot read ambient; it is fixed per run.'
    }

    $psHost = Resolve-PsHost
    $ambStr = $AmbientC.ToString($Invariant)
    $argLine = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden ' +
        "-File `"$harness`" -Label `"$Label`" -AmbientC $ambStr " +
        "-WindowMinutes $WindowMinutes -DwellSeconds $DwellSeconds"
    if (-not [string]::IsNullOrWhiteSpace($Notes)) {
        $argLine += " -Notes `"$Notes`""
    }

    $user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    $action = New-ScheduledTaskAction -Execute $psHost -Argument $argLine -WorkingDirectory $repoRoot
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet `
        -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -StartWhenAvailable -MultipleInstances IgnoreNew `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 10)

    # Repeat every IntervalMinutes for up to a year (remove the task when done),
    # plus an at-logon trigger so it resumes promptly after a reboot.
    $startAt = (Get-Date).AddMinutes(1)
    $repTrigger = New-ScheduledTaskTrigger -Once -At $startAt `
        -RepetitionInterval (New-TimeSpan -Minutes $IntervalMinutes) `
        -RepetitionDuration (New-TimeSpan -Days 365)
    $logonTrigger = New-ScheduledTaskTrigger -AtLogOn -User $user
    $triggers = @($logonTrigger, $repTrigger)

    Register-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath `
        -Action $action -Trigger $triggers -Principal $principal -Settings $settings `
        -Description "Records a CPU temp-by-load baseline data point every $IntervalMinutes min (label '$Label', ambient ${ambStr}C, ${WindowMinutes}-min window) via Compare-CpuTemps.ps1." `
        -Force | Out-Null

    Write-Host "Registered scheduled task: $TaskPath$TaskName" -ForegroundColor Green
    Write-Host "  user    : $user (RunLevel Limited)"
    Write-Host "  cadence : every $IntervalMinutes min, ${WindowMinutes}-min window, dwell ${DwellSeconds}s"
    Write-Host "  label   : $Label   ambient: ${ambStr}C"
    Write-Host "  action  : $psHost $argLine"

    if (-not $NoStart) {
        Start-ScheduledTask -TaskName $TaskName -TaskPath $TaskPath
        Write-Host "Started first run now." -ForegroundColor Green
        Start-Sleep -Seconds 3
    }
    Show-Status
}
