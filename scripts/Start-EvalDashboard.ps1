[CmdletBinding()]
param(
    [int]$Port = 8765,
    [string]$HostName = '127.0.0.1',
    [string]$RuntimeHome = '',
    [switch]$Open,
    [Alias('h')][switch]$Help
)

if ($Help) {
    Write-Host @"
Start-EvalDashboard.ps1 - Serve the local SVG-MB-Control eval dashboard.

USAGE
    .\scripts\Start-EvalDashboard.ps1 [-Port 8765] [-HostName 127.0.0.1] [-RuntimeHome .\release\runtime] [-Open]

The dashboard is static. It reads selected CSV, JSON, and JSONL files in the
browser and does not expose a controller API. The server binds to localhost by
default and serves the repo root. By default it auto-loads packaged runtime
logs under release\runtime; -RuntimeHome points the API endpoints at another
runtime directory. Large live CSVs are exposed through a bounded tail endpoint
so the browser does not parse an entire long-running log.
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
$DashboardServer = Join-Path $DashboardRoot 'server.py'
if (-not (Test-Path -LiteralPath $DashboardServer)) {
    throw "Dashboard server not found at $DashboardServer"
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

    throw "Python was not found on PATH. Install Python or run a static HTTP server rooted at $RepoRoot."
}

$pythonExe = Resolve-Python
$ResolvedRuntimeHome = $RuntimeHome
if ([string]::IsNullOrWhiteSpace($ResolvedRuntimeHome)) {
    $ResolvedRuntimeHome = Join-Path $RepoRoot 'release\runtime'
} elseif (-not [System.IO.Path]::IsPathRooted($ResolvedRuntimeHome)) {
    $ResolvedRuntimeHome = Join-Path $RepoRoot $ResolvedRuntimeHome
}
$ResolvedRuntimeHome = [System.IO.Path]::GetFullPath($ResolvedRuntimeHome)
$url = "http://$HostName`:$Port/tools/eval_dashboard/"

Write-Host "SVG-MB-Control eval dashboard: $url"
Write-Host "Serving repo root: $RepoRoot"
Write-Host "Reading runtime home: $ResolvedRuntimeHome"
Write-Host "Press Ctrl+C to stop."

if ($Open) {
    Start-Process $url
}

if ([System.IO.Path]::GetFileNameWithoutExtension($pythonExe) -ieq 'py') {
    & $pythonExe -3 $DashboardServer --repo-root $RepoRoot --runtime-home $ResolvedRuntimeHome --host $HostName --port $Port
} else {
    & $pythonExe $DashboardServer --repo-root $RepoRoot --runtime-home $ResolvedRuntimeHome --host $HostName --port $Port
}

exit $LASTEXITCODE
