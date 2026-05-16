[CmdletBinding()]
param(
    [int]$Port = 8765,
    [string]$HostName = '127.0.0.1',
    [switch]$Open,
    [Alias('h')][switch]$Help
)

if ($Help) {
    Write-Host @"
Start-EvalDashboard.ps1 - Serve the local SVG-MB-Control eval dashboard.

USAGE
    .\scripts\Start-EvalDashboard.ps1 [-Port 8765] [-HostName 127.0.0.1] [-Open]

The dashboard is static. It reads selected CSV, JSON, and JSONL files in the
browser and does not expose a controller API. The server binds to localhost by
default and serves only tools\eval_dashboard.
"@
    return
}

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$DashboardRoot = Join-Path $RepoRoot 'tools\eval_dashboard'
if (-not (Test-Path -LiteralPath (Join-Path $DashboardRoot 'index.html'))) {
    throw "Dashboard files not found under $DashboardRoot"
}

function Resolve-Python {
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) {
        return $python.Source
    }

    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py) {
        return $py.Source
    }

    throw "Python was not found on PATH. Install Python or run a static HTTP server rooted at $DashboardRoot."
}

$pythonExe = Resolve-Python
$url = "http://$HostName`:$Port/"

Write-Host "SVG-MB-Control eval dashboard: $url"
Write-Host "Serving: $DashboardRoot"
Write-Host "Press Ctrl+C to stop."

if ($Open) {
    Start-Process $url
}

if ([System.IO.Path]::GetFileNameWithoutExtension($pythonExe) -ieq 'py') {
    & $pythonExe -3 -m http.server $Port --bind $HostName --directory $DashboardRoot
} else {
    & $pythonExe -m http.server $Port --bind $HostName --directory $DashboardRoot
}

exit $LASTEXITCODE
