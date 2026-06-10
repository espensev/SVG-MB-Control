# Register the unattended CPU-energy quarantine-exit capture sessions (#2, #3)
# plus a boot/logon safety-revert, under \SVG-MB Control\. Run elevated.
#
# Resilience (so a reboot / missed window does not waste the gate):
#  - StartWhenAvailable: a window missed because the PC was off/asleep runs when
#    the PC is next available.
#  - WakeToRun: wakes the PC from sleep to run.
#  - RestartCount/RestartInterval: retries a session that fails transiently.
#  - Safety-revert (AtStartup + AtLogon): forces energy/cycles OFF after any
#    restart so an interrupted session can never leave energy enabled.
#  - The worker task already auto-starts AtLogon, so after a reboot the controller
#    and HWiNFO (Autorun) come back on their own for the next session.
#  - Span margin: session #3 defaults to +8 days so the >=7-day gate holds even
#    if a session slips a day.
[CmdletBinding()]
param(
    [int]$Session2OffsetDays = 4,
    [int]$Session3OffsetDays = 8,
    [string]$TimeOfDay = '04:00',
    [int]$LoadThreads = 28,        # leave headroom so the 250 ms loop is not starved
    [int]$IdleSeconds = 300,
    [int]$LoadSeconds = 720,
    [int]$CooldownSeconds = 300,
    [string]$SynthLoadExe
)
$ErrorActionPreference = 'Stop'

$id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object System.Security.Principal.WindowsPrincipal($id)).IsInRole(
        [System.Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run elevated: registering tasks with RunLevel Highest needs admin.'
}

$Here = $PSScriptRoot
$Repo = Split-Path -Parent $Here
$wrapper = Join-Path $Here 'Invoke-EnergySessionCapture.ps1'
$reset = Join-Path $Here 'Reset-EnergyToDisabled.ps1'
$pwsh = (Get-Command pwsh -ErrorAction SilentlyContinue).Source
if (-not $pwsh) { $pwsh = (Get-Process -Id $PID).Path }
$user = "$env:USERDOMAIN\$env:USERNAME"
$folder = '\SVG-MB Control\'

# Stage a PERSISTENT synth-load exe (%TEMP% is wiped before +4 days).
if (-not $SynthLoadExe) {
    $stable = Join-Path $Repo 'release\runtime\experiments\energy-quarantine\cpu-synth-load.exe'
    New-Item -ItemType Directory -Force -Path (Split-Path $stable) | Out-Null
    if (-not (Test-Path $stable)) {
        $src = Join-Path $env:TEMP 'cpu-synth-load.exe'
        if (Test-Path $src) { Copy-Item $src $stable -Force }
        else { throw "no synth-load exe to stage (build it first): $stable" }
    }
    $SynthLoadExe = $stable
}
Write-Host "Synth load exe : $SynthLoadExe"

$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$sessSettings = New-ScheduledTaskSettingsSet -WakeToRun -StartWhenAvailable `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Hours 1) `
    -RestartCount 2 -RestartInterval (New-TimeSpan -Minutes 10) `
    -MultipleInstances IgnoreNew

function Register-Session {
    param([int]$Num, [datetime]$When)
    $arg = "-NoProfile -ExecutionPolicy Bypass -File `"$wrapper`" -SessionNum $Num " +
           "-IdleSeconds $IdleSeconds -LoadSeconds $LoadSeconds " +
           "-CooldownSeconds $CooldownSeconds -LoadThreads $LoadThreads " +
           "-SynthLoadExe `"$SynthLoadExe`""
    $action = New-ScheduledTaskAction -Execute $pwsh -Argument $arg -WorkingDirectory $Repo
    $trigger = New-ScheduledTaskTrigger -Once -At $When
    Register-ScheduledTask -TaskPath $folder -TaskName "SVG-MB Energy Session $Num" `
        -Action $action -Trigger $trigger -Settings $sessSettings -Principal $principal `
        -Description "Unattended CPU-energy quarantine capture session $Num" -Force | Out-Null
    Write-Host "registered 'SVG-MB Energy Session $Num' -> $When"
}

# Boot/logon safety-revert.
$safetyAction = New-ScheduledTaskAction -Execute $pwsh `
    -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$reset`"" -WorkingDirectory $Repo
$safetyTriggers = @((New-ScheduledTaskTrigger -AtStartup),
                    (New-ScheduledTaskTrigger -AtLogOn -User $user))
$safetySettings = New-ScheduledTaskSettingsSet -StartWhenAvailable `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 10) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskPath $folder -TaskName 'SVG-MB Energy Safety Revert' `
    -Action $safetyAction -Trigger $safetyTriggers -Settings $safetySettings `
    -Principal $principal -Description 'Force CPU energy/cycle path OFF after any restart' `
    -Force | Out-Null
Write-Host "registered 'SVG-MB Energy Safety Revert' (AtStartup + AtLogon)"

$base = (Get-Date).Date
$t = [timespan]::Parse($TimeOfDay)
Register-Session 2 ($base.AddDays($Session2OffsetDays).Add($t))
Register-Session 3 ($base.AddDays($Session3OffsetDays).Add($t))

$span = $Session3OffsetDays
Write-Host "`nSpan: session 1 today .. session 3 in $span days (gate needs >=7). LoadThreads=$LoadThreads."
