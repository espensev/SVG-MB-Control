#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$TaskName = 'SVG-MB Control Watchdog',
    [string]$TaskPath = '\SVG-MB Control\',
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

function Resolve-TaskRunner {
    param([Parameter(Mandatory = $true)][string]$ExePath)

    $exeDir = Split-Path -Parent $ExePath
    $candidate = Join-Path $exeDir 'svg-mb-control-task-runner.exe'
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        return (Resolve-Path -LiteralPath $candidate).ProviderPath
    }

    return $null
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

function Get-WatchdogTask {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path
    )

    Get-ScheduledTask -TaskName $Name -TaskPath $Path -ErrorAction SilentlyContinue
}

function Invoke-ControlCommand {
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

function Invoke-WatchdogRun {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath,
        [Parameter(Mandatory = $true)][string]$ConfigPath
    )

    $health = Invoke-ControlCommand `
        -ExePath $ExePath `
        -Arguments @('--health', '--json', '--config', $ConfigPath)

    $payload = $null
    $text = ($health.Output -join "`n").Trim()
    if (-not [string]::IsNullOrWhiteSpace($text)) {
        try {
            $payload = $text | ConvertFrom-Json
        } catch {
            Write-Warning "Health output was not valid JSON: $text"
        }
    }

    $state = if ($payload -and $payload.health_state) {
        [string]$payload.health_state
    } else {
        switch ($health.ExitCode) {
            0 { 'healthy' }
            1 { 'degraded' }
            2 { 'stale' }
            3 { 'failed' }
            default { 'failed' }
        }
    }

    Write-Host ("health: {0} (exit {1})" -f $state, $health.ExitCode)
    if ($payload -and $payload.reason) {
        Write-Host "reason: $($payload.reason)"
    }

    if ($health.ExitCode -eq 0 -or $health.ExitCode -eq 1) {
        return 0
    }

    if ($health.ExitCode -eq 2) {
        Write-Host 'watchdog: restarting SVG-MB Control'
        $restart = Invoke-ControlCommand `
            -ExePath $ExePath `
            -Arguments @('--restart', '--config', $ConfigPath)
        foreach ($line in $restart.Output) {
            Write-Host $line
        }
        return $restart.ExitCode
    }

    Write-Warning 'watchdog: failed health is not auto-restarted'
    return $health.ExitCode
}

function Show-WatchdogStatus {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath,
        [Parameter(Mandatory = $true)][string]$ConfigPath,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $task = Get-WatchdogTask -Name $Name -Path $Path
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

    $health = Invoke-ControlCommand `
        -ExePath $ExePath `
        -Arguments @('--health', '--json', '--config', $ConfigPath)
    foreach ($line in $health.Output) {
        Write-Host $line
    }
    Write-Host "health_exit_code: $($health.ExitCode)"
}

$taskPathValue = Normalize-TaskPath -Path $TaskPath
$actionRequested = $Install -or $Run -or $Status -or $Remove
if (-not $actionRequested) {
    $Install = $true
}

if ($Remove -and ($Install -or $Run -or $Status)) {
    throw '-Remove cannot be combined with install/run/status actions.'
}

$exePath = Resolve-ControlExe
$configPath = Resolve-ControlConfig -ExePath $exePath
$taskRunnerPath = Resolve-TaskRunner -ExePath $exePath

if ($Run) {
    exit (Invoke-WatchdogRun -ExePath $exePath -ConfigPath $configPath)
}

if ($Remove) {
    $existing = Get-WatchdogTask -Name $TaskName -Path $taskPathValue
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
    $scriptPath = (Resolve-Path -LiteralPath $PSCommandPath).ProviderPath
    if ($taskRunnerPath) {
        $actionExe = $taskRunnerPath
        $arguments = "--watchdog-run --config `"$configPath`""
    } else {
        $powerShellExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        if (-not (Test-Path -LiteralPath $powerShellExe -PathType Leaf)) {
            $powerShellExe = (Get-Command powershell.exe -ErrorAction Stop).Source
        }
        $launcherPath = Join-Path (Split-Path -Parent $scriptPath) 'Run-SVG-MB-ControlWatchdogHidden.vbs'
        if (Test-Path -LiteralPath $launcherPath -PathType Leaf) {
            $actionExe = Join-Path $env:SystemRoot 'System32\wscript.exe'
            if (-not (Test-Path -LiteralPath $actionExe -PathType Leaf)) {
                $actionExe = (Get-Command wscript.exe -ErrorAction Stop).Source
            }
            $arguments = "//B //NoLogo `"$launcherPath`""
        } else {
            $actionExe = $powerShellExe
            $arguments = "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$scriptPath`" -Run"
        }
    }

    $action = New-ScheduledTaskAction `
        -Execute $actionExe `
        -Argument $arguments `
        -WorkingDirectory (Split-Path -Parent $exePath)
    $logonTrigger = New-ScheduledTaskTrigger -AtLogOn -User $effectiveUser
    $repeatTrigger = New-ScheduledTaskTrigger `
        -Once `
        -At (Get-Date).AddMinutes(1) `
        -RepetitionInterval (New-TimeSpan -Minutes $IntervalMinutes) `
        -RepetitionDuration (New-TimeSpan -Days 3650)
    $principal = New-ScheduledTaskPrincipal `
        -UserId $effectiveUser `
        -LogonType Interactive `
        -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet `
        -AllowStartIfOnBatteries `
        -DontStopIfGoingOnBatteries `
        -StartWhenAvailable `
        -MultipleInstances IgnoreNew `
        -ExecutionTimeLimit (New-TimeSpan -Minutes 2)

    Register-ScheduledTask `
        -TaskName $TaskName `
        -TaskPath $taskPathValue `
        -Action $action `
        -Trigger @($logonTrigger, $repeatTrigger) `
        -Principal $principal `
        -Settings $settings `
        -Description 'Checks SVG-MB Control health and restarts stale or stopped runtimes.' `
        -Force | Out-Null

    Write-Host "Registered scheduled task: $taskPathValue$TaskName"
    Write-Host "  user: $effectiveUser"
    Write-Host "  interval_minutes: $IntervalMinutes"
    Write-Host "  action: $actionExe $arguments"

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
