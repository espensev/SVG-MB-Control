#requires -Version 5.1

# Default scheduled-task identity for the install/watchdog scripts. Centralized
# here so both installers share one source of truth. scripts/Build-Release.ps1
# mirrors the combined watchdog path as a literal for its build-time suspend and
# must track these names.
$SvgMbControlTaskPath  = '\SVG-MB Control\'
$SvgMbControlTaskName  = 'SVG-MB Control'
$SvgMbWatchdogTaskName = 'SVG-MB Control Watchdog'

function Resolve-SvgMbControlExe {
    param([Parameter(Mandatory = $true)][string]$ScriptPath)

    $scriptRoot = Split-Path -Parent $ScriptPath
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

function Resolve-SvgMbControlConfig {
    param([Parameter(Mandatory = $true)][string]$ExePath)

    $exeDir = Split-Path -Parent $ExePath
    $configPath = Join-Path $exeDir 'control.json'
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
        throw "Could not find control.json next to svg-mb-control.exe: $configPath"
    }

    return (Resolve-Path -LiteralPath $configPath).ProviderPath
}

function Resolve-SvgMbControlTaskRunner {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath,
        [switch]$Required
    )

    $exeDir = Split-Path -Parent $ExePath
    $candidate = Join-Path $exeDir 'svg-mb-control-task-runner.exe'
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return (Resolve-Path -LiteralPath $candidate).ProviderPath
    }

    if ($Required) {
        throw "Could not find svg-mb-control-task-runner.exe next to svg-mb-control.exe: $candidate"
    }

    return $null
}

function Get-SvgMbCurrentUserId {
    param([string]$UserId)

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

function Normalize-SvgMbTaskPath {
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

function Invoke-SvgMbControlExe {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$FailureMessage
    )

    $result = Invoke-SvgMbControlCommand -ExePath $ExePath -Arguments $Arguments
    foreach ($line in $result.Output) {
        Write-Host $line
    }
    if ($result.ExitCode -ne 0) {
        throw "$FailureMessage (exit code: $($result.ExitCode))."
    }
}

function Invoke-SvgMbControlCommand {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    $exeDir = Split-Path -Parent $ExePath
    Push-Location -LiteralPath $exeDir
    try {
        $output = & $ExePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
        return [pscustomobject]@{
            ExitCode = $exitCode
            Output = @($output)
        }
    } finally {
        Pop-Location
    }
}

function Get-SvgMbScheduledTask {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path
    )

    Get-ScheduledTask -TaskName $Name -TaskPath $Path -ErrorAction SilentlyContinue
}

function Write-SvgMbTaskInfo {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $task = Get-SvgMbScheduledTask -Name $Name -Path $Path
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
}

function Register-SvgMbControlTask {
    param(
        [Parameter(Mandatory = $true)][string]$EffectiveUser,
        [Parameter(Mandatory = $true)][string]$TaskName,
        [Parameter(Mandatory = $true)][string]$TaskPath,
        [Parameter(Mandatory = $true)][string]$ExecuteExe,
        [Parameter(Mandatory = $true)][string]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)]$Triggers,
        [Parameter(Mandatory = $true)][timespan]$ExecutionTimeLimit,
        [Parameter(Mandatory = $true)][string]$Description,
        [string[]]$ExtraInfoLines = @()
    )

    $action = New-ScheduledTaskAction `
        -Execute $ExecuteExe `
        -Argument $Arguments `
        -WorkingDirectory $WorkingDirectory
    $principal = New-ScheduledTaskPrincipal `
        -UserId $EffectiveUser `
        -LogonType Interactive `
        -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet `
        -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries `
        -StartWhenAvailable `
        -MultipleInstances IgnoreNew `
        -ExecutionTimeLimit $ExecutionTimeLimit

    Register-ScheduledTask `
        -TaskName $TaskName `
        -TaskPath $TaskPath `
        -Action $action `
        -Trigger $Triggers `
        -Principal $principal `
        -Settings $settings `
        -Description $Description `
        -Force | Out-Null

    Write-Host "Registered scheduled task: $TaskPath$TaskName"
    Write-Host "  user: $EffectiveUser"
    foreach ($line in $ExtraInfoLines) {
        Write-Host "  $line"
    }
    Write-Host "  action: $ExecuteExe $Arguments"
}
