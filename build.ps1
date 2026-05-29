<#
.SYNOPSIS
    Run the documented local CI-style validation workflow.

.DESCRIPTION
    This root convenience entry point intentionally delegates to
    scripts\Test-LocalCI.ps1 instead of setting up a separate Visual Studio or
    CMake path. The build directory is kept by default so local failures can be
    inspected without rerunning configure/build.

.EXAMPLE
    .\build.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$RepoRoot = $PSScriptRoot
$LocalCiScript = Join-Path $RepoRoot 'scripts\Test-LocalCI.ps1'
if (-not (Test-Path -LiteralPath $LocalCiScript -PathType Leaf)) {
    throw "Local CI script not found: $LocalCiScript"
}

& $LocalCiScript -KeepBuildDir
exit $LASTEXITCODE
