#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$TaskName,
    [string]$TaskPath,
    [string]$UserId,
    [ValidateRange(1, 30)]
    [int]$IntervalMinutes = 1,
    [switch]$Install,
    [switch]$Run,
    [switch]$Status,
    [switch]$Remove,
    [switch]$NoStart
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$commonScript = Join-Path (Split-Path -Parent $PSCommandPath) 'Install-SVG-MB-ControlCommon.ps1'
if (-not (Test-Path -LiteralPath $commonScript -PathType Leaf)) {
    throw "Common installer helpers not found: $commonScript"
}
. $commonScript

if ([string]::IsNullOrWhiteSpace($TaskName)) { $TaskName = $SvgMbWatchdogTaskName }
if ([string]::IsNullOrWhiteSpace($TaskPath)) { $TaskPath = $SvgMbControlTaskPath }

function Show-WatchdogStatus {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath,
        [Parameter(Mandatory = $true)][string]$ConfigPath,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path
    )

    Write-SvgMbTaskInfo -Name $Name -Path $Path

    $health = Invoke-SvgMbControlCommand `
        -ExePath $ExePath `
        -Arguments @('--health', '--json', '--config', $ConfigPath)
    foreach ($line in $health.Output) {
        Write-Host $line
    }
    Write-Host "health_exit_code: $($health.ExitCode)"
}

$taskPathValue = Normalize-SvgMbTaskPath -Path $TaskPath
$actionRequested = $Install -or $Run -or $Status -or $Remove
if (-not $actionRequested) {
    $Install = $true
}

if ($Remove -and ($Install -or $Run -or $Status)) {
    throw '-Remove cannot be combined with install/run/status actions.'
}

if ($Remove) {
    $existing = Get-SvgMbScheduledTask -Name $TaskName -Path $taskPathValue
    if ($existing) {
        Unregister-ScheduledTask -TaskName $TaskName -TaskPath $taskPathValue -Confirm:$false
        Write-Host "Removed scheduled task: $taskPathValue$TaskName"
    } else {
        Write-Host "Scheduled task already absent: $taskPathValue$TaskName"
    }
    return
}

$exePath = Resolve-SvgMbControlExe -ScriptPath $PSCommandPath
$configPath = Resolve-SvgMbControlConfig -ExePath $exePath

if ($Run) {
    # Delegate to the same native runner the scheduled action uses so the
    # health-check and restart policy live only in src/platform/task_runner.cpp.
    $taskRunnerPath = Resolve-SvgMbControlTaskRunner -ExePath $exePath -Required
    $result = Invoke-SvgMbControlCommand `
        -ExePath $taskRunnerPath `
        -Arguments @('--watchdog-run', '--config', $configPath)
    foreach ($line in $result.Output) {
        Write-Host $line
    }
    exit $result.ExitCode
}

if ($Install) {
    $effectiveUser = Get-SvgMbCurrentUserId -UserId $UserId
    $taskRunnerPath = Resolve-SvgMbControlTaskRunner -ExePath $exePath -Required
    $arguments = "--watchdog-run --config `"$configPath`""

    $logonTrigger = New-ScheduledTaskTrigger -AtLogOn -User $effectiveUser
    $repeatTrigger = New-ScheduledTaskTrigger `
        -Once `
        -At (Get-Date).AddMinutes(1) `
        -RepetitionInterval (New-TimeSpan -Minutes $IntervalMinutes) `
        -RepetitionDuration (New-TimeSpan -Days 3650)

    Register-SvgMbControlTask `
        -EffectiveUser $effectiveUser `
        -TaskName $TaskName `
        -TaskPath $taskPathValue `
        -ExecuteExe $taskRunnerPath `
        -Arguments $arguments `
        -WorkingDirectory (Split-Path -Parent $exePath) `
        -Triggers @($logonTrigger, $repeatTrigger) `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 2) `
        -Description 'Checks SVG-MB Control health and restarts stale or stopped runtimes.' `
        -ExtraInfoLines @("interval_minutes: $IntervalMinutes")

    if (-not $NoStart) {
        Start-ScheduledTask -TaskName $TaskName -TaskPath $taskPathValue
        Write-Host "Started scheduled task: $taskPathValue$TaskName"
        Start-Sleep -Seconds 2
    }
}

if ($Status -or $Install) {
    Show-WatchdogStatus `
        -ExePath $exePath `
        -ConfigPath $configPath `
        -Name $TaskName `
        -Path $taskPathValue
}
