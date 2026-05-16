#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$TaskName = 'SVG-MB Control',
    [string]$TaskPath = '\SVG-MB Control\',
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

function Resolve-ControlExe {
    $scriptRoot = Split-Path -Parent $PSCommandPath
    $candidates = @(
        (Join-Path $scriptRoot 'svg-mb-control.exe'),
        (Join-Path $scriptRoot 'release\svg-mb-control.exe')
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).ProviderPath
        }
    }

    throw 'Could not find svg-mb-control.exe next to this script or under release\.'
}

function Resolve-ControlConfig {
    param([Parameter(Mandatory = $true)][string]$ExePath)

    $exeDir = Split-Path -Parent $ExePath
    $configPath = Join-Path $exeDir 'control.json'
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
        throw "Could not find control.json next to svg-mb-control.exe: $configPath"
    }

    return (Resolve-Path -LiteralPath $configPath).ProviderPath
}

function Get-CurrentUserId {
    if (-not [string]::IsNullOrWhiteSpace($UserId)) {
        return $UserId
    }

    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    if ($identity -and -not [string]::IsNullOrWhiteSpace($identity.Name)) {
        return $identity.Name
    }

    if (-not [string]::IsNullOrWhiteSpace($env:USERDOMAIN) -and
        -not [string]::IsNullOrWhiteSpace($env:USERNAME)) {
        return "$env:USERDOMAIN\$env:USERNAME"
    }

    throw 'Could not resolve the current Windows user.'
}

function Normalize-TaskPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $normalized = $Path.Trim()
    if ([string]::IsNullOrWhiteSpace($normalized)) {
        $normalized = '\'
    }
    if (-not $normalized.StartsWith('\')) {
        $normalized = "\$normalized"
    }
    if (-not $normalized.EndsWith('\')) {
        $normalized = "$normalized\"
    }
    return $normalized
}

function Get-ControlTask {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path
    )

    Get-ScheduledTask -TaskName $Name -TaskPath $Path -ErrorAction SilentlyContinue
}

function Invoke-ControlExe {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$FailureMessage
    )

    $exeDir = Split-Path -Parent $ExePath
    Push-Location -LiteralPath $exeDir
    try {
        & $ExePath @Arguments
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0) {
            throw "$FailureMessage (exit code: $exitCode)."
        }
    } finally {
        Pop-Location
    }
}

function Show-ControlStatus {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $task = Get-ControlTask -Name $Name -Path $Path
    if ($task) {
        $info = Get-ScheduledTaskInfo -TaskName $Name -TaskPath $Path
        Write-Host "Scheduled task: $Path$Name"
        Write-Host "  state: $($task.State)"
        Write-Host "  last_run: $($info.LastRunTime)"
        Write-Host "  last_result: $($info.LastTaskResult)"
        Write-Host "  next_run: $($info.NextRunTime)"
    } else {
        Write-Host "Scheduled task: not installed ($Path$Name)"
    }

    Invoke-ControlExe -ExePath $ExePath -Arguments @('--status') -FailureMessage 'Status command failed'
}

$taskPathValue = Normalize-TaskPath -Path $TaskPath
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

$exePath = Resolve-ControlExe
$configPath = Resolve-ControlConfig -ExePath $exePath
$exeDir = Split-Path -Parent $exePath

if ($Remove) {
    $existing = Get-ControlTask -Name $TaskName -Path $taskPathValue
    if ($existing) {
        Unregister-ScheduledTask -TaskName $TaskName -TaskPath $taskPathValue -Confirm:$false
        Write-Host "Removed scheduled task: $taskPathValue$TaskName"
    } else {
        Write-Host "Scheduled task already absent: $taskPathValue$TaskName"
    }
    return
}

if ($Install) {
    $effectiveUser = Get-CurrentUserId
    $arguments = "--start --config `"$configPath`""
    $action = New-ScheduledTaskAction `
        -Execute $exePath `
        -Argument $arguments `
        -WorkingDirectory $exeDir
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $effectiveUser
    $principal = New-ScheduledTaskPrincipal `
        -UserId $effectiveUser `
        -LogonType Interactive `
        -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet `
        -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries `
        -StartWhenAvailable `
        -MultipleInstances IgnoreNew `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 5)

    Register-ScheduledTask `
        -TaskName $TaskName `
        -TaskPath $taskPathValue `
        -Action $action `
        -Trigger $trigger `
        -Principal $principal `
        -Settings $settings `
        -Description 'Starts SVG-MB Control fan controller at user logon.' `
        -Force | Out-Null

    Write-Host "Registered scheduled task: $taskPathValue$TaskName"
    Write-Host "  user: $effectiveUser"
    Write-Host "  action: $exePath $arguments"

    if (-not $NoStart) {
        $Start = $true
    }
}

if ($Restart) {
    Invoke-ControlExe -ExePath $exePath -Arguments @('--restart', '--config', $configPath) -FailureMessage 'Restart command failed'
    Show-ControlStatus -ExePath $exePath -Name $TaskName -Path $taskPathValue
    return
}

if ($Stop) {
    Invoke-ControlExe -ExePath $exePath -Arguments @('--stop', '--config', $configPath) -FailureMessage 'Stop command failed'
}

if ($Start) {
    $task = Get-ControlTask -Name $TaskName -Path $taskPathValue
    if ($task) {
        Start-ScheduledTask -TaskName $TaskName -TaskPath $taskPathValue
        Write-Host "Started scheduled task: $taskPathValue$TaskName"
        Start-Sleep -Seconds 2
    } else {
        Invoke-ControlExe -ExePath $exePath -Arguments @('--start', '--config', $configPath) -FailureMessage 'Start command failed'
    }
}

if ($Status -or $Start -or $Stop -or $Install) {
    Show-ControlStatus -ExePath $exePath -Name $TaskName -Path $taskPathValue
}
