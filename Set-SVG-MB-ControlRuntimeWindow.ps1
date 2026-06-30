#requires -Version 5.1
<#
.SYNOPSIS
    Manage intentional SVG-MB-Control stop/pause/restart windows.

.DESCRIPTION
    Provides one operator entrypoint for bounded off windows. A pause disables
    the controller and watchdog scheduled tasks, requests a cooperative stop,
    optionally starts read-only evidence-log capture, and registers a one-shot
    resume task. Resume stops the optional evidence logger before starting
    Control again so the shared stop request cannot stop the resumed controller.

    -DryRun prints the planned actions and writes nothing.
#>
[CmdletBinding(DefaultParameterSetName = 'Status')]
param(
    [Parameter(ParameterSetName = 'Pause', Mandatory = $true)]
    [switch]$Pause,

    [Parameter(ParameterSetName = 'Stop', Mandatory = $true)]
    [switch]$Stop,

    [Parameter(ParameterSetName = 'Resume', Mandatory = $true)]
    [switch]$Resume,

    [Parameter(ParameterSetName = 'Restart', Mandatory = $true)]
    [switch]$Restart,

    [Parameter(ParameterSetName = 'Status')]
    [switch]$Status,

    [Parameter(ParameterSetName = 'Pause')]
    [Parameter(ParameterSetName = 'Stop')]
    [Alias('For')]
    [string]$Duration,

    [Parameter(ParameterSetName = 'Pause')]
    [Parameter(ParameterSetName = 'Stop')]
    [datetime]$Until,

    [Parameter(ParameterSetName = 'Pause')]
    [Parameter(ParameterSetName = 'Stop')]
    [switch]$EvidenceLog,

    [string]$TaskName,
    [string]$TaskPath,
    [string]$WatchdogTaskName,
    [string]$ResumeTaskName = 'SVG-MB Control Resume Window',
    [string]$EvidenceTaskName = 'SVG-MB Control Evidence During Window',
    [string]$StatePath,
    [string]$ExePath,
    [string]$ConfigPath,

    [ValidateRange(5, 300)]
    [int]$EvidenceStopTimeoutSec = 30,

    [switch]$DryRun,
    [switch]$Json,
    [switch]$NoElevate,
    [Alias('h')]
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Invariant = [System.Globalization.CultureInfo]::InvariantCulture

if ($Help) {
    Write-Host @"
Set-SVG-MB-ControlRuntimeWindow.ps1 - Manage intentional runtime windows.

USAGE
  .\Set-SVG-MB-ControlRuntimeWindow.ps1 -Status
  .\Set-SVG-MB-ControlRuntimeWindow.ps1 -Status -Json
  .\Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -For 1h
  .\Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -For 45m -EvidenceLog
  .\Set-SVG-MB-ControlRuntimeWindow.ps1 -Stop
  .\Set-SVG-MB-ControlRuntimeWindow.ps1 -Resume
  .\Set-SVG-MB-ControlRuntimeWindow.ps1 -Restart

DURATION
  -For accepts 30s, 15m, 1h, 2h30m, 1d, or a TimeSpan like 01:30:00.
  Use -Until <datetime> when an exact resume time is clearer.

LOGGING
  -EvidenceLog starts read-only evidence-log during a bounded off window and
  stops it before resume. It writes svg_mb_control_evidence.* files, separate
  from the normal control-loop CSV/events.

SAFETY
  This script never writes fan duty directly and never resets breakers.
  -DryRun prints the exact task/runtime actions without changing the system.

COORDINATION
  -Status -Json emits helper/task/window state as machine-readable JSON for
  external coordinators. Other repos should invoke this packaged script or the
  packaged exe as a process boundary, not import code from this repo.
"@
    return
}

if (-not ($Pause -or $Stop -or $Resume -or $Restart -or $Status)) {
    $Status = $true
}
if ($Json -and -not $Status) {
    throw '-Json is only supported with -Status.'
}

$scriptRoot = Split-Path -Parent $PSCommandPath
$commonScript = Join-Path $scriptRoot 'Install-SVG-MB-ControlCommon.ps1'
if (-not (Test-Path -LiteralPath $commonScript -PathType Leaf)) {
    throw "Common installer helpers not found: $commonScript"
}
. $commonScript

if ([string]::IsNullOrWhiteSpace($TaskName)) { $TaskName = $SvgMbControlTaskName }
if ([string]::IsNullOrWhiteSpace($TaskPath)) { $TaskPath = $SvgMbControlTaskPath }
if ([string]::IsNullOrWhiteSpace($WatchdogTaskName)) { $WatchdogTaskName = $SvgMbWatchdogTaskName }
$TaskPath = Normalize-SvgMbTaskPath -Path $TaskPath

function Test-Admin {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object System.Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole(
        [System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

function ConvertTo-NativeQuotedArgument {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)

    if ($Value -notmatch '[\s"]' -and -not $Value.EndsWith('\')) {
        return $Value
    }

    $quoted = '"'
    $backslashes = 0
    foreach ($ch in $Value.ToCharArray()) {
        if ($ch -eq '\') {
            $backslashes += 1
            continue
        }
        if ($ch -eq '"') {
            $quoted += ('\' * (($backslashes * 2) + 1))
            $quoted += '"'
            $backslashes = 0
            continue
        }
        $quoted += ('\' * $backslashes)
        $backslashes = 0
        $quoted += $ch
    }
    $quoted += ('\' * ($backslashes * 2))
    $quoted += '"'
    return $quoted
}

function Join-NativeArguments {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    return (($Arguments | ForEach-Object { ConvertTo-NativeQuotedArgument $_ }) -join ' ')
}

function Resolve-PathForOperator {
    param(
        [string]$Path,
        [string]$Description
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ''
    }
    $full = [System.IO.Path]::GetFullPath($Path)
    if ((Test-Path -LiteralPath $full -PathType Leaf) -or $DryRun) {
        return $full
    }
    throw "$Description not found: $full"
}

function Resolve-ControlExePath {
    if (-not [string]::IsNullOrWhiteSpace($ExePath)) {
        return Resolve-PathForOperator -Path $ExePath -Description 'Control executable'
    }
    return Resolve-SvgMbControlExe -ScriptPath $PSCommandPath
}

function Resolve-ControlConfigPath {
    param([Parameter(Mandatory = $true)][string]$ResolvedExePath)

    if (-not [string]::IsNullOrWhiteSpace($ConfigPath)) {
        return Resolve-PathForOperator -Path $ConfigPath -Description 'Control config'
    }
    if ($DryRun -and -not (Test-Path -LiteralPath $ResolvedExePath -PathType Leaf)) {
        return [System.IO.Path]::GetFullPath(
            (Join-Path (Split-Path -Parent $ResolvedExePath) 'control.json'))
    }
    return Resolve-SvgMbControlConfig -ExePath $ResolvedExePath
}

function Resolve-RuntimeHome {
    param(
        [Parameter(Mandatory = $true)][string]$ResolvedExePath,
        [Parameter(Mandatory = $true)][string]$ResolvedConfigPath
    )

    $fallback = Join-Path (Split-Path -Parent $ResolvedExePath) 'runtime'
    if (-not (Test-Path -LiteralPath $ResolvedConfigPath -PathType Leaf)) {
        return [System.IO.Path]::GetFullPath($fallback)
    }
    try {
        $config = Get-Content -Raw -LiteralPath $ResolvedConfigPath | ConvertFrom-Json
        $configured = [string]$config.runtime_home_path
        if ([string]::IsNullOrWhiteSpace($configured)) {
            return [System.IO.Path]::GetFullPath($fallback)
        }
        if ([System.IO.Path]::IsPathRooted($configured)) {
            return [System.IO.Path]::GetFullPath($configured)
        }
        return [System.IO.Path]::GetFullPath(
            (Join-Path (Split-Path -Parent $ResolvedConfigPath) $configured))
    } catch {
        return [System.IO.Path]::GetFullPath($fallback)
    }
}

function ConvertTo-WindowDuration {
    param([string]$Text)

    if ([string]::IsNullOrWhiteSpace($Text)) {
        throw 'Provide -For <duration> or -Until <datetime> for a bounded window.'
    }

    $parsed = [TimeSpan]::Zero
    if ([TimeSpan]::TryParse($Text, $Invariant, [ref]$parsed)) {
        if ($parsed.TotalSeconds -gt 0) { return $parsed }
    }

    $matches = [regex]::Matches($Text, '(?i)(\d+(?:\.\d+)?)(d|h|m|s)')
    if ($matches.Count -eq 0) {
        throw "Could not parse duration '$Text'. Use examples like 30m, 1h, 2h30m, or 01:30:00."
    }

    $covered = (($matches | ForEach-Object { $_.Value }) -join '')
    if ($covered.Length -ne ($Text -replace '\s+', '').Length) {
        throw "Could not parse duration '$Text'."
    }

    $seconds = 0.0
    foreach ($match in $matches) {
        $value = [double]::Parse($match.Groups[1].Value, $Invariant)
        switch ($match.Groups[2].Value.ToLowerInvariant()) {
            'd' { $seconds += $value * 86400.0 }
            'h' { $seconds += $value * 3600.0 }
            'm' { $seconds += $value * 60.0 }
            's' { $seconds += $value }
        }
    }
    if ($seconds -le 0.0) {
        throw "Duration must be greater than zero: $Text"
    }
    return [TimeSpan]::FromSeconds($seconds)
}

function Resolve-ResumeAt {
    if ($PSBoundParameters.ContainsKey('Until')) {
        if ($Until -le (Get-Date)) {
            throw "-Until must be in the future. Got: $Until"
        }
        return $Until
    }
    if ([string]::IsNullOrWhiteSpace($Duration)) {
        if ($Pause) {
            throw '-Pause requires -For <duration> or -Until <datetime>.'
        }
        return $null
    }
    return (Get-Date).Add((ConvertTo-WindowDuration -Text $Duration))
}

function Invoke-OrEcho {
    param(
        [Parameter(Mandatory = $true)][string]$Description,
        [Parameter(Mandatory = $true)][scriptblock]$Action
    )
    if ($DryRun) {
        Write-Host "  [dry-run] $Description" -ForegroundColor DarkGray
        return
    }
    Write-Host "  $Description" -ForegroundColor Cyan
    & $Action
}

function Get-TaskEnabledState {
    param([string]$Name, [string]$Path)

    if ($DryRun) {
        return $null
    }

    try {
        $record = Get-SvgMbSchtasksRecord -Name $Name -Path $Path
    } catch {
        $record = $null
    }
    if ($record) {
        return [string]::Equals(
            [string]$record.'Scheduled Task State',
            'Enabled',
            [System.StringComparison]::OrdinalIgnoreCase)
    }

    $task = Get-SvgMbScheduledTask -Name $Name -Path $Path
    if ($task -and $task.Settings -and $null -ne $task.Settings.Enabled) {
        return [bool]$task.Settings.Enabled
    }
    return $null
}

function Test-TaskInstalledSafe {
    param([string]$Name, [string]$Path)

    try {
        return [bool](Test-SvgMbScheduledTaskInstalled -Name $Name -Path $Path)
    } catch {
        return $false
    }
}

function Set-TaskEnabledState {
    param(
        [string]$Name,
        [string]$Path,
        [bool]$Enabled
    )

    $state = if ($Enabled) { 'enable' } else { 'disable' }
    $fullName = Get-SvgMbTaskFullName -Name $Name -Path $Path
    Invoke-OrEcho "$state scheduled task $fullName" {
        if (-not (Test-TaskInstalledSafe -Name $Name -Path $Path)) {
            Write-Host "    task not installed: $fullName"
            return
        }
        try {
            if ($Enabled) {
                Enable-ScheduledTask -TaskName $Name -TaskPath $Path -ErrorAction Stop | Out-Null
            } else {
                Disable-ScheduledTask -TaskName $Name -TaskPath $Path -ErrorAction Stop | Out-Null
            }
            return
        } catch {
            $switch = if ($Enabled) { '/ENABLE' } else { '/DISABLE' }
            & schtasks.exe /Change /TN $fullName $switch | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "Could not $state scheduled task: $fullName"
            }
        }
    }
}

function Stop-TaskIfRunning {
    param([string]$Name, [string]$Path)

    $fullName = Get-SvgMbTaskFullName -Name $Name -Path $Path
    Invoke-OrEcho "stop scheduled task action $fullName" {
        if (-not (Test-TaskInstalledSafe -Name $Name -Path $Path)) {
            Write-Host "    task not installed: $fullName"
            return
        }
        try {
            Stop-ScheduledTask -TaskName $Name -TaskPath $Path -ErrorAction Stop
        } catch {
            & schtasks.exe /End /TN $fullName 2>$null | Out-Null
        }
    }
}

function Remove-TaskIfInstalled {
    param([string]$Name, [string]$Path)

    $fullName = Get-SvgMbTaskFullName -Name $Name -Path $Path
    Invoke-OrEcho "remove scheduled task $fullName" {
        if (Test-TaskInstalledSafe -Name $Name -Path $Path) {
            Remove-SvgMbScheduledTaskCompat -Name $Name -Path $Path
        }
    }
}

function Invoke-ControlCommand {
    param(
        [string]$ResolvedExePath,
        [string[]]$Arguments,
        [string]$Description
    )

    Invoke-OrEcho "$Description`: $ResolvedExePath $(Join-NativeArguments -Arguments $Arguments)" {
        $result = Invoke-SvgMbControlCommand -ExePath $ResolvedExePath -Arguments $Arguments
        foreach ($line in $result.Output) { Write-Host $line }
        if ($result.ExitCode -ne 0) {
            throw "$Description failed (exit code: $($result.ExitCode))."
        }
    }
}

function New-OperatorWindowState {
    param(
        $ResumeAt,
        [string]$ResolvedExePath,
        [string]$ResolvedConfigPath,
        [string]$ResolvedRuntimeHome,
        [object]$MainTaskEnabled,
        [object]$WatchdogEnabled
    )

    $windowId = (Get-Date).ToString('yyyyMMdd-HHmmss')
    return [pscustomobject]@{
        schema_version = 1
        window_id = $windowId
        state = 'active'
        created_at = (Get-Date).ToString('s')
        resume_at = if ($null -ne $ResumeAt) { $ResumeAt.ToString('s') } else { '' }
        script_path = [System.IO.Path]::GetFullPath($PSCommandPath)
        exe_path = $ResolvedExePath
        config_path = $ResolvedConfigPath
        runtime_home = $ResolvedRuntimeHome
        task_path = $TaskPath
        task_name = $TaskName
        watchdog_task_name = $WatchdogTaskName
        resume_task_name = $ResumeTaskName
        evidence_task_name = $EvidenceTaskName
        main_task_was_enabled = $MainTaskEnabled
        watchdog_task_was_enabled = $WatchdogEnabled
        evidence_log = [bool]$EvidenceLog
        evidence_stop_timeout_sec = $EvidenceStopTimeoutSec
    }
}

function Write-OperatorWindowState {
    param(
        [Parameter(Mandatory = $true)]$State,
        [Parameter(Mandatory = $true)][string]$Path
    )

    Invoke-OrEcho "write operator window state $Path" {
        $dir = Split-Path -Parent $Path
        if (-not (Test-Path -LiteralPath $dir)) {
            New-Item -ItemType Directory -Path $dir -Force | Out-Null
        }
        $State | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Path -Encoding ASCII
    }
}

function Read-OperatorWindowState {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }
    return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
}

function New-TaskStatusSummary {
    param([string]$Name, [string]$Path)

    $installed = $null
    $enabled = $null
    if (-not $DryRun) {
        $installed = Test-TaskInstalledSafe -Name $Name -Path $Path
        if ($installed) {
            $enabled = Get-TaskEnabledState -Name $Name -Path $Path
        }
    }

    return [pscustomobject]@{
        name = $Name
        path = $Path
        full_name = Get-SvgMbTaskFullName -Name $Name -Path $Path
        installed = $installed
        enabled = $enabled
    }
}

function Write-StatusJson {
    $state = Read-OperatorWindowState -Path $StatePath
    [pscustomobject]@{
        schema_version = 1
        helper = 'Set-SVG-MB-ControlRuntimeWindow.ps1'
        mode = 'status'
        dry_run = [bool]$DryRun
        sibling_repo_dependency = $false
        coordinator_contract = 'process-boundary'
        script_path = [System.IO.Path]::GetFullPath($PSCommandPath)
        exe_path = $resolvedExePath
        config_path = $resolvedConfigPath
        runtime_home = $runtimeHome
        state_path = $StatePath
        active_window = $state
        tasks = [pscustomobject]@{
            control = New-TaskStatusSummary -Name $TaskName -Path $TaskPath
            watchdog = New-TaskStatusSummary -Name $WatchdogTaskName -Path $TaskPath
            resume = New-TaskStatusSummary -Name $ResumeTaskName -Path $TaskPath
            evidence = New-TaskStatusSummary -Name $EvidenceTaskName -Path $TaskPath
        }
    } | ConvertTo-Json -Depth 8
}

function Register-ResumeTask {
    param(
        [datetime]$ResumeAt,
        [string]$ResolvedStatePath
    )

    $psHost = (Get-Process -Id $PID).Path
    $args = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-WindowStyle', 'Hidden',
        '-File', [System.IO.Path]::GetFullPath($PSCommandPath),
        '-Resume',
        '-StatePath', $ResolvedStatePath,
        '-NoElevate'
    )
    $argLine = Join-NativeArguments -Arguments $args
    $fullName = Get-SvgMbTaskFullName -Name $ResumeTaskName -Path $TaskPath
    Invoke-OrEcho "register one-shot resume task $fullName at $($ResumeAt.ToString('s'))" {
        $action = New-ScheduledTaskAction -Execute $psHost -Argument $argLine -WorkingDirectory $scriptRoot
        $trigger = New-ScheduledTaskTrigger -Once -At $ResumeAt
        $principal = New-ScheduledTaskPrincipal -UserId (Get-SvgMbCurrentUserId '') -LogonType Interactive -RunLevel Highest
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
        Register-ScheduledTask -TaskName $ResumeTaskName -TaskPath $TaskPath -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Description 'Resumes SVG-MB Control after an intentional operator runtime window.' -Force | Out-Null
    }
}

function Register-EvidenceLogTask {
    param(
        [datetime]$ResumeAt,
        [string]$ResolvedExePath,
        [string]$ResolvedConfigPath
    )

    $secondsUntilResume = [Math]::Max(60, [int][Math]::Ceiling(($ResumeAt - (Get-Date)).TotalSeconds))
    $limit = [TimeSpan]::FromSeconds($secondsUntilResume + 600)
    $argLine = Join-NativeArguments -Arguments @('--mode', 'evidence-log', '--config', $ResolvedConfigPath)
    $fullName = Get-SvgMbTaskFullName -Name $EvidenceTaskName -Path $TaskPath
    Invoke-OrEcho "register evidence-log task $fullName`: $ResolvedExePath $argLine" {
        $action = New-ScheduledTaskAction -Execute $ResolvedExePath -Argument $argLine -WorkingDirectory (Split-Path -Parent $ResolvedExePath)
        $trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddSeconds(3)
        $principal = New-ScheduledTaskPrincipal -UserId (Get-SvgMbCurrentUserId '') -LogonType Interactive -RunLevel Highest
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit $limit
        Register-ScheduledTask -TaskName $EvidenceTaskName -TaskPath $TaskPath -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Description 'Read-only evidence-log capture during an intentional SVG-MB Control off window.' -Force | Out-Null
        Start-SvgMbScheduledTaskCompat -Name $EvidenceTaskName -Path $TaskPath
    }
}

function Get-EvidenceLogProcesses {
    param(
        [string]$ResolvedExePath,
        [string]$ResolvedConfigPath
    )

    $exeFull = [System.IO.Path]::GetFullPath($ResolvedExePath)
    $configFull = [System.IO.Path]::GetFullPath($ResolvedConfigPath)
    @(Get-CimInstance Win32_Process -Filter "Name='svg-mb-control.exe'" -ErrorAction SilentlyContinue |
        Where-Object {
            $_.ExecutablePath -and
            [string]::Equals([System.IO.Path]::GetFullPath($_.ExecutablePath), $exeFull, [System.StringComparison]::OrdinalIgnoreCase) -and
            $_.CommandLine -and
            $_.CommandLine.Contains('--mode') -and
            $_.CommandLine.Contains('evidence-log') -and
            $_.CommandLine.Contains($configFull)
        })
}

function Stop-EvidenceLogIfNeeded {
    param($State)

    if (-not $State -or -not [bool]$State.evidence_log) {
        return
    }

    $resolvedExe = [string]$State.exe_path
    $resolvedConfig = [string]$State.config_path
    Invoke-ControlCommand -ResolvedExePath $resolvedExe -Arguments @('--stop', '--config', $resolvedConfig) -Description 'request evidence-log stop'

    if ($DryRun) { return }

    $deadline = (Get-Date).AddSeconds([int]$State.evidence_stop_timeout_sec)
    while ((Get-Date) -lt $deadline) {
        $procs = Get-EvidenceLogProcesses -ResolvedExePath $resolvedExe -ResolvedConfigPath $resolvedConfig
        if ($procs.Count -eq 0) { break }
        Start-Sleep -Milliseconds 500
    }

    $remaining = Get-EvidenceLogProcesses -ResolvedExePath $resolvedExe -ResolvedConfigPath $resolvedConfig
    if ($remaining.Count -gt 0) {
        Write-Warning "Evidence-log did not exit before timeout; forcing remaining evidence-log process(es)."
        foreach ($proc in $remaining) {
            Stop-Process -Id $proc.ProcessId -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-TaskIfInstalled -Name ([string]$State.evidence_task_name) -Path ([string]$State.task_path)
}

function Stop-ControlForWindow {
    param(
        [string]$ResolvedExePath,
        [string]$ResolvedConfigPath
    )

    Stop-TaskIfRunning -Name $WatchdogTaskName -Path $TaskPath
    Set-TaskEnabledState -Name $WatchdogTaskName -Path $TaskPath -Enabled:$false
    Stop-TaskIfRunning -Name $TaskName -Path $TaskPath
    Set-TaskEnabledState -Name $TaskName -Path $TaskPath -Enabled:$false
    Invoke-ControlCommand -ResolvedExePath $ResolvedExePath -Arguments @('--stop', '--config', $ResolvedConfigPath) -Description 'request controller stop'
}

function Resume-ControlFromState {
    param(
        $State,
        [string]$FallbackExePath,
        [string]$FallbackConfigPath
    )

    $resolvedExe = if ($State -and $State.exe_path) { [string]$State.exe_path } else { $FallbackExePath }
    $resolvedConfig = if ($State -and $State.config_path) { [string]$State.config_path } else { $FallbackConfigPath }
    $stateTaskPath = if ($State -and $State.task_path) { [string]$State.task_path } else { $TaskPath }
    $stateTaskName = if ($State -and $State.task_name) { [string]$State.task_name } else { $TaskName }
    $stateWatchdogName = if ($State -and $State.watchdog_task_name) { [string]$State.watchdog_task_name } else { $WatchdogTaskName }
    $stateResumeName = if ($State -and $State.resume_task_name) { [string]$State.resume_task_name } else { $ResumeTaskName }

    Stop-EvidenceLogIfNeeded -State $State

    $restoreMainTask = $true
    if ($State -and $null -ne $State.main_task_was_enabled) {
        $restoreMainTask = [bool]$State.main_task_was_enabled
    }
    $restoreWatchdogTask = $true
    if ($State -and $null -ne $State.watchdog_task_was_enabled) {
        $restoreWatchdogTask = [bool]$State.watchdog_task_was_enabled
    }

    if ($restoreMainTask) {
        Set-TaskEnabledState -Name $stateTaskName -Path $stateTaskPath -Enabled:$true
        Invoke-OrEcho "start controller task $(Get-SvgMbTaskFullName -Name $stateTaskName -Path $stateTaskPath)" {
            if (Test-TaskInstalledSafe -Name $stateTaskName -Path $stateTaskPath) {
                Start-SvgMbScheduledTaskCompat -Name $stateTaskName -Path $stateTaskPath
                Start-Sleep -Seconds 2
            } else {
                Invoke-SvgMbControlExe -ExePath $resolvedExe -Arguments @('--start', '--config', $resolvedConfig) -FailureMessage 'Start command failed'
            }
        }
    } else {
        Invoke-ControlCommand -ResolvedExePath $resolvedExe -Arguments @('--start', '--config', $resolvedConfig) -Description 'start controller directly'
    }

    if ($restoreWatchdogTask) {
        Set-TaskEnabledState -Name $stateWatchdogName -Path $stateTaskPath -Enabled:$true
    }

    Remove-TaskIfInstalled -Name $stateResumeName -Path $stateTaskPath
}

$resolvedExePath = Resolve-ControlExePath
$resolvedConfigPath = Resolve-ControlConfigPath -ResolvedExePath $resolvedExePath
$runtimeHome = Resolve-RuntimeHome -ResolvedExePath $resolvedExePath -ResolvedConfigPath $resolvedConfigPath
if ([string]::IsNullOrWhiteSpace($StatePath)) {
    $StatePath = Join-Path (Join-Path $runtimeHome 'operator_windows') 'active_window.json'
} elseif (-not [System.IO.Path]::IsPathRooted($StatePath)) {
    $StatePath = [System.IO.Path]::GetFullPath($StatePath)
}

$mutating = $Pause -or $Stop -or $Resume -or $Restart
if ($mutating -and -not $DryRun -and -not $NoElevate -and -not (Test-Admin)) {
    Write-Host 'Re-launching elevated (scheduled-task control needs admin)...' -ForegroundColor Yellow
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-NoElevate')
    foreach ($name in 'Pause','Stop','Resume','Restart','Status','EvidenceLog','DryRun','Json') {
        if ((Get-Variable -Name $name -ValueOnly -ErrorAction SilentlyContinue)) {
            $args += "-$name"
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($Duration)) { $args += @('-For', $Duration) }
    if ($PSBoundParameters.ContainsKey('Until')) { $args += @('-Until', $Until.ToString('o')) }
    foreach ($pair in @(
        @('TaskName', $TaskName),
        @('TaskPath', $TaskPath),
        @('WatchdogTaskName', $WatchdogTaskName),
        @('ResumeTaskName', $ResumeTaskName),
        @('EvidenceTaskName', $EvidenceTaskName),
        @('StatePath', $StatePath),
        @('ExePath', $resolvedExePath),
        @('ConfigPath', $resolvedConfigPath)
    )) {
        if (-not [string]::IsNullOrWhiteSpace([string]$pair[1])) {
            $args += @("-$($pair[0])", [string]$pair[1])
        }
    }
    $args += @('-EvidenceStopTimeoutSec', [string]$EvidenceStopTimeoutSec)
    Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList $args -Verb RunAs
    return
}

if ($Status -and $Json) {
    Write-StatusJson
    return
}

Write-Host "SVG-MB Control runtime window"
Write-Host "  exe         : $resolvedExePath"
Write-Host "  config      : $resolvedConfigPath"
Write-Host "  runtime_home: $runtimeHome"
Write-Host "  state       : $StatePath"

if ($Status) {
    Write-SvgMbTaskInfo -Name $TaskName -Path $TaskPath
    Write-SvgMbTaskInfo -Name $WatchdogTaskName -Path $TaskPath
    Write-SvgMbTaskInfo -Name $ResumeTaskName -Path $TaskPath
    Write-SvgMbTaskInfo -Name $EvidenceTaskName -Path $TaskPath
    if (Test-Path -LiteralPath $StatePath -PathType Leaf) {
        Write-Host "Operator window state:"
        Get-Content -Raw -LiteralPath $StatePath | Write-Host
    } else {
        Write-Host "Operator window state: none"
    }
    Invoke-ControlCommand -ResolvedExePath $resolvedExePath -Arguments @('--status', '--config', $resolvedConfigPath) -Description 'read controller status'
    return
}

if ($Restart) {
    Invoke-ControlCommand -ResolvedExePath $resolvedExePath -Arguments @('--restart', '--config', $resolvedConfigPath) -Description 'restart controller'
    return
}

if ($Pause -or $Stop) {
    $resumeAt = Resolve-ResumeAt
    if ($EvidenceLog -and -not $resumeAt) {
        throw '-EvidenceLog requires a bounded window (-Pause, or -Stop with -For/-Until).'
    }

    $mainEnabled = Get-TaskEnabledState -Name $TaskName -Path $TaskPath
    $watchdogEnabled = Get-TaskEnabledState -Name $WatchdogTaskName -Path $TaskPath
    $state = New-OperatorWindowState `
        -ResumeAt $resumeAt `
        -ResolvedExePath $resolvedExePath `
        -ResolvedConfigPath $resolvedConfigPath `
        -ResolvedRuntimeHome $runtimeHome `
        -MainTaskEnabled $mainEnabled `
        -WatchdogEnabled $watchdogEnabled

    Write-OperatorWindowState -State $state -Path $StatePath
    Stop-ControlForWindow -ResolvedExePath $resolvedExePath -ResolvedConfigPath $resolvedConfigPath

    if ($EvidenceLog) {
        Register-EvidenceLogTask -ResumeAt $resumeAt -ResolvedExePath $resolvedExePath -ResolvedConfigPath $resolvedConfigPath
    }
    if ($resumeAt) {
        Register-ResumeTask -ResumeAt $resumeAt -ResolvedStatePath $StatePath
        Write-Host "Window active until: $($resumeAt.ToString('s'))"
    } else {
        Write-Host 'Controller stopped with tasks disabled. Use -Resume to start it again.'
    }
    return
}

if ($Resume) {
    $state = Read-OperatorWindowState -Path $StatePath
    if (-not $state) {
        Write-Host 'No operator-window state file found; resuming with current defaults.' -ForegroundColor Yellow
    }
    Resume-ControlFromState -State $state -FallbackExePath $resolvedExePath -FallbackConfigPath $resolvedConfigPath
    if ($state -and -not $DryRun) {
        $lastPath = Join-Path (Split-Path -Parent $StatePath) 'last_window.json'
        $state.state = 'completed'
        $state.completed_at = (Get-Date).ToString('s')
        $state | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $lastPath -Encoding ASCII
        Remove-Item -LiteralPath $StatePath -Force -ErrorAction SilentlyContinue
    }
    Write-Host 'Resume complete.'
    return
}
