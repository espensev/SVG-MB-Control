# Capture one FEAT-0006 CPU-energy quarantine-exit evidence session, mostly
# unattended. Operationalizes docs/cpu-energy-quarantine-exit-capture-runbook-
# 2026-06-10.md: snapshot a fresh disabled baseline, enable the read-only RAPL
# energy (and by default cycle) path, restart the live worker TREE so the new
# env propagates, verify the quarantine marker, drive idle -> synthetic load ->
# cooldown while harvesting the SMU (HWiNFO) + RAPL (LHM) reference sensors,
# then ALWAYS revert to disabled and restart. Emits a manifest JSON for
# score_energy_session.py.
#
# Live Runtime Safety: read-only telemetry only (no fan/MSR/CPU writes). The
# enable/revert each restart the worker once (brief control gap) -- run at idle.
# The finally block reverts to disabled even on error so energy is never left on.
#
# This is throwaway capture tooling (not shipped, not the control loop). It is
# the ONLY piece here that touches the live controller; -Rehearse skips every
# live action (no enable, no restart) and just exercises the profile + harvest.
#
# Usage (elevated; self-elevates if not):
#   .\scripts\Capture-EnergySession.ps1                      # full real session
#   .\scripts\Capture-EnergySession.ps1 -Rehearse            # no-touch rehearsal
#   .\scripts\Capture-EnergySession.ps1 -IdleSeconds 60 -LoadSeconds 420 -CooldownSeconds 120
[CmdletBinding()]
param(
    [int]$IdleSeconds = 300,
    [int]$LoadSeconds = 720,       # >= 6 min crosses a 32-bit energy wrap
    [int]$CooldownSeconds = 300,
    [int]$LoadThreads = 0,         # 0 = all logical processors
    [switch]$EnergyOnly,           # cycles are captured too unless this is set
    [switch]$Rehearse,             # run profile + harvest WITHOUT enable/restart
    [string]$OutDir,
    [string]$SynthLoadExe,
    [string]$SessionLabel = 'energy-quarantine',
    [switch]$NoElevate             # internal: set on the self-elevated relaunch
)

$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ReleaseRoot = Join-Path $RepoRoot 'release'
$LiveCsv = Join-Path $ReleaseRoot 'runtime\logs\svg_mb_control_output.csv'
$TaskPath = '\SVG-MB Control\'
$TaskName = 'SVG-MB Control'
$EnergyVar = 'SVG_MB_CONTROL_RAPL_ENERGY_MODE'
$CyclesVar = 'SVG_MB_CONTROL_CPU_CYCLES_MODE'
$MarkerCol = 'cpu_pkg_energy_acquisition'

function Test-Admin {
    $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object System.Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Self-elevate for scheduled-task control unless rehearsing (rehearsal is
# read-only and needs no rights).
if (-not $Rehearse -and -not $NoElevate -and -not (Test-Admin)) {
    Write-Host 'Re-launching elevated (scheduled-task control needs admin)...' -ForegroundColor Yellow
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-NoElevate',
        '-IdleSeconds', $IdleSeconds, '-LoadSeconds', $LoadSeconds, '-CooldownSeconds', $CooldownSeconds,
        '-LoadThreads', $LoadThreads, '-SessionLabel', $SessionLabel)
    if ($EnergyOnly) { $argList += '-EnergyOnly' }
    if ($OutDir) { $argList += @('-OutDir', $OutDir) }
    if ($SynthLoadExe) { $argList += @('-SynthLoadExe', $SynthLoadExe) }
    Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList $argList -Verb RunAs
    return
}

# ---- helpers ---------------------------------------------------------------

function Get-CsvColumnIndex {
    param([string]$Csv, [string]$Name)
    if (-not (Test-Path $Csv)) { return -1 }
    foreach ($line in (Get-Content $Csv -TotalCount 60)) {
        if ($line.StartsWith('#') -or [string]::IsNullOrWhiteSpace($line)) { continue }
        $cols = $line -split ','
        for ($i = 0; $i -lt $cols.Count; $i++) {
            if ($cols[$i].Trim() -eq $Name) { return $i }
        }
        return -1   # first non-comment line is the header; stop here
    }
    return -1
}

function Get-LiveMarker {
    param([string]$Csv)
    $idx = Get-CsvColumnIndex -Csv $Csv -Name $MarkerCol
    if ($idx -lt 0) { return $null }
    $tail = Get-Content $Csv -Tail 4 -ErrorAction SilentlyContinue
    for ($k = $tail.Count - 1; $k -ge 0; $k--) {
        $line = $tail[$k]
        if ($line.StartsWith('#') -or [string]::IsNullOrWhiteSpace($line)) { continue }
        $cols = $line -split ','
        if ($idx -lt $cols.Count) { return $cols[$idx].Trim() }
    }
    return $null
}

function Restart-WorkerTree {
    param([string]$Reason)
    Write-Host "  restart worker tree ($Reason)" -ForegroundColor Cyan
    try { Stop-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -ErrorAction Stop } catch {
        Write-Warning "Stop-ScheduledTask: $($_.Exception.Message)"
    }
    # Task-runner may spawn a detached supervisor tree; kill the release\ worker
    # processes so the relaunched tree inherits the freshly-set env.
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        $procs = Get-CimInstance Win32_Process -Filter "Name='svg-mb-control.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -and $_.CommandLine -like "*$ReleaseRoot*" }
        if (-not $procs) { break }
        foreach ($p in $procs) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Milliseconds 500
    }
    Start-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName
}

function Wait-ForMarker {
    param([string]$Want, [int]$TimeoutSec = 90)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $m = Get-LiveMarker -Csv $LiveCsv
        if ($m -eq $Want) { return $true }
        Start-Sleep -Seconds 2
    }
    return $false
}

function Resolve-SynthLoad {
    if ($SynthLoadExe -and (Test-Path $SynthLoadExe)) { return $SynthLoadExe }
    $candidates = @(
        (Join-Path $RepoRoot 'build\x64-release\cpu-synth-load.exe'),
        (Join-Path $env:TEMP 'cpu-synth-load.exe')
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    Write-Host '  synth-load exe not found; building it...' -ForegroundColor Yellow
    . (Join-Path $PSScriptRoot 'Build.VsEnv.ps1')
    Import-VsDevShellEnvironment -VsInstallPath (Get-VsInstallPath) -VsInstanceId (Get-VsInstanceId) -Arch amd64 -HostArch amd64
    $out = Join-Path $env:TEMP 'cpu-synth-load.exe'
    $src = Join-Path $RepoRoot 'tools\cpu_synth_load.cpp'
    Push-Location $env:TEMP
    try { & cl /nologo /O2 /arch:AVX2 /EHsc /std:c++20 /W4 $src "/Fe:$out" | Out-Null }
    finally { Pop-Location }
    if (-not (Test-Path $out)) { throw 'failed to build cpu-synth-load.exe' }
    return $out
}

function Now-Iso { (Get-Date).ToString('yyyy-MM-ddTHH:mm:ss') }

# ---- pre-flight ------------------------------------------------------------

if (-not (Get-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -ErrorAction SilentlyContinue)) {
    throw "Worker task '$TaskPath$TaskName' not found."
}
if (-not (Get-Process HWiNFO64 -ErrorAction SilentlyContinue)) {
    Write-Warning 'HWiNFO64 is not running -> the SMU criterion-3 reference will be blank. Start HWiNFO (Scribe) first.'
}

if (-not $OutDir) {
    $stamp = (Get-Date).ToString('yyyyMMdd_HHmmss')
    $OutDir = Join-Path $ReleaseRoot "runtime\experiments\energy-quarantine\$stamp"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$harvester = Join-Path $PSScriptRoot 'harvest_reference_sensors.py'
$refCsv = Join-Path $OutDir 'reference_sensors.csv'
$baselineCsv = Join-Path $OutDir 'baseline_disabled.csv'
$sessionCsv = Join-Path $OutDir 'session.csv'
$manifestPath = Join-Path $OutDir 'manifest.json'
$includeCycles = -not $EnergyOnly
$totalSeconds = $IdleSeconds + $LoadSeconds + $CooldownSeconds + 30

Write-Host "=== Capture-EnergySession ($SessionLabel) ===" -ForegroundColor Green
Write-Host "OutDir         : $OutDir"
Write-Host "Profile        : idle ${IdleSeconds}s -> load ${LoadSeconds}s -> cooldown ${CooldownSeconds}s"
Write-Host "Cycles         : $includeCycles    Rehearse: $Rehearse"

$synth = Resolve-SynthLoad
Write-Host "Synth load exe : $synth"

# Fresh disabled baseline (criterion-6 reference): snapshot the current live CSV
# BEFORE any enable. The scorer computes loop_slip/overrun from it.
if (Test-Path $LiveCsv) {
    Copy-Item $LiveCsv $baselineCsv -Force
    Write-Host "Baseline snap  : $baselineCsv ($([math]::Round((Get-Item $baselineCsv).Length/1MB,1)) MB)"
} else {
    Write-Warning "Live CSV not found at $LiveCsv; baseline skipped."
}

$origEnergy = [Environment]::GetEnvironmentVariable($EnergyVar, 'User')
$origCycles = [Environment]::GetEnvironmentVariable($CyclesVar, 'User')

$manifest = [ordered]@{
    session_label   = $SessionLabel
    created         = Now-Iso
    rehearse        = [bool]$Rehearse
    include_cycles  = [bool]$includeCycles
    repo_root       = $RepoRoot
    out_dir         = $OutDir
    synth_load_exe  = $synth
    load_threads    = $LoadThreads
    baseline_csv    = (Test-Path $baselineCsv) ? $baselineCsv : $null
    reference_csv   = $refCsv
    session_csv     = $sessionCsv
    phases          = [ordered]@{}
    markers         = [ordered]@{}
}

$enabled = $false
$harvProc = $null
try {
    # ---- enable + verify ----
    if (-not $Rehearse) {
        Write-Host "`n[enable] setting env + restarting worker tree..." -ForegroundColor Yellow
        [Environment]::SetEnvironmentVariable($EnergyVar, 'enabled', 'User')
        if ($includeCycles) { [Environment]::SetEnvironmentVariable($CyclesVar, 'enabled', 'User') }
        $enabled = $true
        Restart-WorkerTree -Reason 'enable energy'
        if (-not (Wait-ForMarker -Want 'quarantine' -TimeoutSec 90)) {
            throw "marker did not reach 'quarantine' after enable+restart (env propagation failed?) -- reverting."
        }
        $manifest.markers.after_enable = Get-LiveMarker -Csv $LiveCsv
        Write-Host "  marker = $($manifest.markers.after_enable)" -ForegroundColor Green
    } else {
        Write-Host "`n[rehearse] skipping enable/restart; running against current worker." -ForegroundColor Yellow
        $manifest.markers.after_enable = Get-LiveMarker -Csv $LiveCsv
    }

    # ---- start harvester for the whole session ----
    Write-Host "[harvest] starting reference-sensor harvester (${totalSeconds}s)..." -ForegroundColor Yellow
    $harvArgs = @($harvester, '--seconds', $totalSeconds, '--interval', '1', '--out', $refCsv, '--quiet')
    $harvProc = Start-Process -FilePath 'python' -ArgumentList $harvArgs -PassThru -WindowStyle Hidden

    # ---- idle ----
    $manifest.phases.idle_start = Now-Iso
    Write-Host "[idle] ${IdleSeconds}s..." -ForegroundColor Yellow
    Start-Sleep -Seconds $IdleSeconds
    $manifest.phases.idle_end = Now-Iso

    # ---- load (steady window excludes 1st/last for spin-up/down) ----
    $manifest.phases.load_start = Now-Iso
    Write-Host "[load] synthetic ${LoadSeconds}s ($([math]::Max(1,$LoadThreads)) or all threads)..." -ForegroundColor Yellow
    $loadArgs = @('--seconds', $LoadSeconds)
    if ($LoadThreads -gt 0) { $loadArgs += @('--threads', $LoadThreads) }
    & $synth @loadArgs
    $manifest.phases.load_end = Now-Iso
    $manifest.phases.steady_start = ([datetime]$manifest.phases.load_start).AddSeconds(120).ToString('yyyy-MM-ddTHH:mm:ss')
    $manifest.phases.steady_end = ([datetime]$manifest.phases.load_end).AddSeconds(-60).ToString('yyyy-MM-ddTHH:mm:ss')

    # ---- cooldown ----
    $manifest.phases.cooldown_start = Now-Iso
    Write-Host "[cooldown] ${CooldownSeconds}s..." -ForegroundColor Yellow
    Start-Sleep -Seconds $CooldownSeconds
    $manifest.phases.cooldown_end = Now-Iso

    if ($harvProc -and -not $harvProc.HasExited) { $harvProc | Wait-Process -Timeout 60 -ErrorAction SilentlyContinue }

    # ---- snapshot the enabled session CSV ----
    if (Test-Path $LiveCsv) { Copy-Item $LiveCsv $sessionCsv -Force }
    $manifest.markers.before_revert = Get-LiveMarker -Csv $LiveCsv
}
finally {
    if ($harvProc -and -not $harvProc.HasExited) {
        $harvProc | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    if ($enabled) {
        Write-Host "`n[revert] restoring env to disabled + restarting worker tree..." -ForegroundColor Yellow
        [Environment]::SetEnvironmentVariable($EnergyVar, ($origEnergy ? $origEnergy : 'disabled'), 'User')
        [Environment]::SetEnvironmentVariable($CyclesVar, ($origCycles ? $origCycles : 'disabled'), 'User')
        Restart-WorkerTree -Reason 'revert to disabled'
        if (Wait-ForMarker -Want 'disabled' -TimeoutSec 90) {
            Write-Host '  reverted: marker = disabled' -ForegroundColor Green
        } else {
            Write-Warning "marker did not return to 'disabled' -- CHECK $EnergyVar and the worker manually."
        }
        $manifest.markers.after_revert = Get-LiveMarker -Csv $LiveCsv
    }
    $manifest.completed = Now-Iso
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -Path $manifestPath -Encoding utf8
    Write-Host "`nManifest -> $manifestPath" -ForegroundColor Green
}

Write-Host "`nDone. Score with:" -ForegroundColor Green
Write-Host "  python scripts\score_energy_session.py --manifest `"$manifestPath`" --session-num <N>" -ForegroundColor Gray
