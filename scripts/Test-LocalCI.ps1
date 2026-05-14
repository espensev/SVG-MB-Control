# Runs the release build plus hermetic smoke tests without stopping a live
# svg-mb-control process.
[CmdletBinding()]
param(
    [switch]$KeepBuildDir
)

$ErrorActionPreference = 'Stop'
$RepoRoot = if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'CMakeLists.txt')) {
    $PSScriptRoot
} else {
    Split-Path -Parent $PSScriptRoot
}

$buildScript = Join-Path $RepoRoot 'scripts\Build-Release.ps1'
if ($KeepBuildDir) {
    & $buildScript -NoStopProcesses -KeepBuildDir
} else {
    & $buildScript -NoStopProcesses
}
exit $LASTEXITCODE
