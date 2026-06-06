# Runs the release build plus hermetic smoke tests without stopping a live
# svg-mb-control process.
#
# A single-instance lock keeps two concurrent runs from colliding on the shared
# build\ tree and the C++ lane's fixed-name %TEMP% artifacts (a collision that
# otherwise surfaces as a flaky "attempting to parse an empty input" failure in
# svg_mb_control_loop_config_tests). A second run refuses by default (exit 75) or
# queues with -Wait. The lock is an exclusively-opened file in the system temp
# directory, keyed to this repo path; the OS releases the handle if the holder
# dies, so a leftover lock file never wedges a future run. Direct
# Build-Release.ps1 runs are not covered on purpose (the publish path keeps its
# behavior) -- use Test-LocalCI for local validation, per AGENTS.md.
[CmdletBinding()]
param(
    [switch]$KeepBuildDir,
    [switch]$Wait,
    [int]$WaitTimeoutMinutes = 30
)

$ErrorActionPreference = 'Stop'
$RepoRoot = if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'CMakeLists.txt')) {
    $PSScriptRoot
} else {
    Split-Path -Parent $PSScriptRoot
}

# Acquire the per-repo single-instance lock. Returns an object holding the open
# FileStream (whose handle IS the lock); the caller disposes it to release. On
# contention this either refuses (exit 75) or, with -Wait, polls until the active
# run finishes or the timeout elapses.
function Open-LocalCiLock {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [switch]$Wait,
        [int]$TimeoutMinutes = 30
    )

    # Key the lock to the canonical repo path so unrelated checkouts do not
    # serialize against each other.
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hashBytes = $sha.ComputeHash(
            [System.Text.Encoding]::UTF8.GetBytes($RepoRoot.ToLowerInvariant()))
    } finally {
        $sha.Dispose()
    }
    $hashHex = -join ($hashBytes[0..7] | ForEach-Object { $_.ToString('x2') })
    $lockPath = Join-Path ([System.IO.Path]::GetTempPath()) "svg_mb_control_localci_$hashHex.lock"

    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    $announced = $false
    while ($true) {
        try {
            $stream = [System.IO.File]::Open(
                $lockPath,
                [System.IO.FileMode]::OpenOrCreate,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None)
        } catch [System.IO.IOException] {
            # FileShare.None is held by another run (same- or cross-process).
            if (-not $Wait) {
                Write-Host "[lock] Another Test-LocalCI run is active for this repo; refusing." -ForegroundColor Yellow
                Write-Host "[lock] Lock file: $lockPath" -ForegroundColor DarkGray
                Write-Host "[lock] Re-run with -Wait to queue behind it." -ForegroundColor DarkGray
                exit 75
            }
            if ((Get-Date) -ge $deadline) {
                Write-Host "[lock] Timed out after $TimeoutMinutes min waiting for the active run." -ForegroundColor Red
                exit 75
            }
            if (-not $announced) {
                Write-Host "[lock] Another Test-LocalCI run is active; waiting (timeout ${TimeoutMinutes}m)..." -ForegroundColor Yellow
                $announced = $true
            }
            Start-Sleep -Seconds 2
            continue
        }
        # Acquired. Record the holder; this is readable only once the lock is
        # released (FileShare.None blocks reads while held), so it is post-mortem
        # diagnostics, not a live status channel.
        try {
            $stream.SetLength(0)
            $info = [System.Text.Encoding]::UTF8.GetBytes(
                "pid=$PID`nstarted=$((Get-Date).ToUniversalTime().ToString('o'))`nrepo=$RepoRoot`n")
            $stream.Write($info, 0, $info.Length)
            $stream.Flush()
        } catch {
            # Best-effort; the open handle alone provides the mutual exclusion.
        }
        return [pscustomobject]@{ Stream = $stream; Path = $lockPath }
    }
}

$lock = Open-LocalCiLock -RepoRoot $RepoRoot -Wait:$Wait -TimeoutMinutes $WaitTimeoutMinutes
$code = 1
try {
    $buildScript = Join-Path $RepoRoot 'scripts\Build-Release.ps1'
    if ($KeepBuildDir) {
        & $buildScript -NoStopProcesses -NoPublish -KeepBuildDir
    } else {
        & $buildScript -NoStopProcesses -NoPublish
    }
    $code = $LASTEXITCODE
} finally {
    if ($lock) { try { $lock.Stream.Dispose() } catch {} }
}
exit $code
