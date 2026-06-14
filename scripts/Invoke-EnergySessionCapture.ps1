# Unattended wrapper: run ONE CPU-energy quarantine-exit capture session, then
# score it and write the evidence note. Invoked by the scheduled tasks that
# Schedule-EnergySessions.ps1 registers (it runs elevated via the task's
# RunLevel Highest, so no UAC). Logs everything to the session's run.log.
#
# Best-effort ensures HWiNFO is up first (the SMU criterion-3 reference); if it
# is not running and Scribe is available it launches it. Read-only telemetry
# only; the orchestrator's finally block reverts energy to disabled on exit.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][int]$SessionNum,
    [int]$IdleSeconds = 300,
    [int]$LoadSeconds = 720,
    [int]$CooldownSeconds = 300,
    [int]$LoadThreads = 0,
    [string]$SynthLoadExe,
    [string]$ScribeExe = 'D:\Development\Thermals\Legacy\hwnfo-logging\Scribe\build\Release\dist\Scribe.exe'
)
$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = Split-Path -Parent $Here
$stamp = (Get-Date).ToString('yyyyMMdd_HHmmss')
$outDir = Join-Path $Repo "release\runtime\experiments\energy-quarantine\session${SessionNum}_$stamp"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$log = Join-Path $outDir 'run.log'

"[$(Get-Date -Format s)] session $SessionNum start" | Tee-Object -FilePath $log

# Ensure HWiNFO is running for the SMU reference (Autorun=1 normally covers it).
if (-not (Get-Process HWiNFO64 -ErrorAction SilentlyContinue)) {
    if (Test-Path $ScribeExe) {
        "[$(Get-Date -Format s)] HWiNFO down; launching Scribe" | Tee-Object -FilePath $log -Append
        Start-Process $ScribeExe
        Start-Sleep -Seconds 25
    } else {
        "[$(Get-Date -Format s)] WARN HWiNFO down and Scribe not found -> SMU ref will blank" |
            Tee-Object -FilePath $log -Append
    }
}

$captureArgs = @{
    NoElevate       = $true
    OutDir          = $outDir
    SessionLabel    = "session$SessionNum-auto"
    IdleSeconds     = $IdleSeconds
    LoadSeconds     = $LoadSeconds
    CooldownSeconds = $CooldownSeconds
    LoadThreads     = $LoadThreads
}
if ($SynthLoadExe) { $captureArgs.SynthLoadExe = $SynthLoadExe }

& (Join-Path $Here 'Capture-EnergySession.ps1') @captureArgs *>> $log

$manifest = Join-Path $outDir 'manifest.json'
if (Test-Path $manifest) {
    $note = Join-Path $Repo ("docs\cpu-energy-quarantine-exit-evidence-{0}-s{1}.md" -f (Get-Date -Format 'yyyy-MM-dd'), $SessionNum)
    python (Join-Path $Here 'score_energy_session.py') --manifest $manifest --session-num $SessionNum --out $note *>> $log
    "[$(Get-Date -Format s)] scored -> $note" | Tee-Object -FilePath $log -Append
} else {
    "[$(Get-Date -Format s)] ERROR no manifest at $manifest" | Tee-Object -FilePath $log -Append
}
"[$(Get-Date -Format s)] session $SessionNum done" | Tee-Object -FilePath $log -Append
