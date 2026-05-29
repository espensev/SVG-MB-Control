#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$TaskName,
    [string]$TaskPath,
    [string]$UserId,
    [switch]$Install,
    [switch]$Start,
    [switch]$Stop,
    [switch]$Restart,
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

if ([string]::IsNullOrWhiteSpace($TaskName)) { $TaskName = $SvgMbControlTaskName }
if ([string]::IsNullOrWhiteSpace($TaskPath)) { $TaskPath = $SvgMbControlTaskPath }

function Show-ControlStatus {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path
    )

    Write-SvgMbTaskInfo -Name $Name -Path $Path
    Invoke-SvgMbControlExe -ExePath $ExePath -Arguments @('--status') -FailureMessage 'Status command failed'
}

$taskPathValue = Normalize-SvgMbTaskPath -Path $TaskPath
$actionRequested = $Install -or $Start -or $Stop -or $Restart -or $Status -or $Remove
if (-not $actionRequested) {
    $Install = $true
}

if ($Remove -and ($Install -or $Start -or $Stop -or $Restart -or $Status)) {
    throw '-Remove cannot be combined with install/start/stop/restart/status actions.'
}
if ($Restart -and ($Start -or $Stop)) {
    throw '-Restart cannot be combined with -Start or -Stop.'
}

$exePath = Resolve-SvgMbControlExe -ScriptPath $PSCommandPath
$configPath = Resolve-SvgMbControlConfig -ExePath $exePath
$exeDir = Split-Path -Parent $exePath

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

if ($Install) {
    $effectiveUser = Get-SvgMbCurrentUserId -UserId $UserId
    $taskRunnerPath = Resolve-SvgMbControlTaskRunner -ExePath $exePath -Required
    $arguments = "--start --config `"$configPath`""
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $effectiveUser

    Register-SvgMbControlTask `
        -EffectiveUser $effectiveUser `
        -TaskName $TaskName `
        -TaskPath $taskPathValue `
        -ExecuteExe $taskRunnerPath `
        -Arguments $arguments `
        -WorkingDirectory $exeDir `
        -Triggers $trigger `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 5) `
        -Description 'Starts SVG-MB Control fan controller at user logon.'

    if (-not $NoStart) {
        $Start = $true
    }
}

if ($Restart) {
    Invoke-SvgMbControlExe -ExePath $exePath -Arguments @('--restart', '--config', $configPath) -FailureMessage 'Restart command failed'
    Show-ControlStatus -ExePath $exePath -Name $TaskName -Path $taskPathValue
    return
}

if ($Stop) {
    Invoke-SvgMbControlExe -ExePath $exePath -Arguments @('--stop', '--config', $configPath) -FailureMessage 'Stop command failed'
}

if ($Start) {
    $task = Get-SvgMbScheduledTask -Name $TaskName -Path $taskPathValue
    if ($task) {
        Start-ScheduledTask -TaskName $TaskName -TaskPath $taskPathValue
        Write-Host "Started scheduled task: $taskPathValue$TaskName"
        Start-Sleep -Seconds 2
    } else {
        Invoke-SvgMbControlExe -ExePath $exePath -Arguments @('--start', '--config', $configPath) -FailureMessage 'Start command failed'
    }
}

if ($Status -or $Start -or $Stop -or $Install) {
    Show-ControlStatus -ExePath $exePath -Name $TaskName -Path $taskPathValue
}
