# Boot/logon SAFETY net for the CPU-energy quarantine capture. Guarantees the
# read-only energy + cycle path is OFF after any restart, so a capture session
# interrupted by a reboot/crash/power-loss can NEVER leave energy enabled
# (decision §Disturbance mitigation: never leave enabled until the gate passes).
#
# Idempotent and fast: always forces the env vars to 'disabled'; only restarts
# the worker if it actually came back up still in 'quarantine' (an interrupted
# session). Registered by Schedule-EnergySessions.ps1 to run AtStartup + AtLogon.
[CmdletBinding()]
param([int]$GraceSeconds = 120)
$ErrorActionPreference = 'Continue'

$Repo = Split-Path -Parent $PSScriptRoot
$expDir = Join-Path $Repo 'release\runtime\experiments\energy-quarantine'
New-Item -ItemType Directory -Force -Path $expDir | Out-Null
$log = Join-Path $expDir 'safety-revert.log'
function Log($m) { "$(Get-Date -Format s)  $m" | Out-File $log -Append -Encoding utf8 }

$EnergyVar = 'SVG_MB_CONTROL_RAPL_ENERGY_MODE'
$CyclesVar = 'SVG_MB_CONTROL_CPU_CYCLES_MODE'
$TaskPath = '\SVG-MB Control\'
$TaskName = 'SVG-MB Control'
$live = Join-Path $Repo 'release\runtime\logs\svg_mb_control_output.csv'

$e0 = [Environment]::GetEnvironmentVariable($EnergyVar, 'User')
$c0 = [Environment]::GetEnvironmentVariable($CyclesVar, 'User')
Log "start; energy='$e0' cycles='$c0'"

# The safety action: force OFF unconditionally. This is the real guarantee --
# any worker launched after this point reads 'disabled'.
[Environment]::SetEnvironmentVariable($EnergyVar, 'disabled', 'User')
[Environment]::SetEnvironmentVariable($CyclesVar, 'disabled', 'User')

function Get-Marker {
    if (-not (Test-Path $live)) { return $null }
    $hdr = (Get-Content $live -TotalCount 40 | Where-Object { $_ -notmatch '^#' } |
        Select-Object -First 1) -split ','
    $i = [array]::IndexOf($hdr, 'cpu_pkg_energy_acquisition')
    if ($i -lt 0) { return $null }
    # The worker appends a fresh header section on every start, so the file
    # can end on a header row (worker died before its first data row,
    # observed 2026-06-11). Scan back to the last data row instead of
    # reading only the final line.
    $tail = @(Get-Content $live -Tail 50)
    for ($n = $tail.Count - 1; $n -ge 0; $n--) {
        $line = $tail[$n]
        if ([string]::IsNullOrWhiteSpace($line) -or $line -match '^#') { continue }
        $f = $line -split ','
        if ($i -lt $f.Count -and $f[$i].Trim() -ne 'cpu_pkg_energy_acquisition') {
            return $f[$i].Trim()
        }
    }
    return $null
}

# If a worker is already up still in quarantine (an interrupted session left the
# stale 'enabled' env in the running tree), restart it so it re-reads 'disabled'.
$deadline = (Get-Date).AddSeconds($GraceSeconds)
$restarted = $false
while ((Get-Date) -lt $deadline) {
    $m = Get-Marker
    if ($m -eq 'disabled') { Log 'marker=disabled; ok'; break }
    if ($m -eq 'quarantine' -and -not $restarted) {
        Log 'marker=quarantine after restart -> restarting worker tree to apply disabled'
        try { Stop-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -ErrorAction Stop } catch {}
        Get-CimInstance Win32_Process -Filter "Name='svg-mb-control.exe'" -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -like "*$Repo\release*" } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Seconds 2
        Start-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName
        $restarted = $true
    }
    Start-Sleep -Seconds 5
}
Log "done; final marker=$(Get-Marker) restarted=$restarted"
