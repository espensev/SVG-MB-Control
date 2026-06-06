#requires -Version 5.1

# External-process, hash, git, and build-tool resolution helpers.
# Extracted from Build-Release.ps1; dot-sourced by it. Functions are
# parameter-driven and invoked from the Build-Release.ps1 pipeline scope.

function Invoke-External {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$FailureMessage
    )

    Write-Host "  > $FilePath $($Arguments -join ' ')" -ForegroundColor DarkGray
    $global:LASTEXITCODE = 0
    try {
        & $FilePath @Arguments
    } catch {
        throw "$FailureMessage ($FilePath): $($_.Exception.Message)"
    }

    $exitCode = $global:LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "$FailureMessage (exit code: $exitCode)."
    }
}

function Resolve-CTestPath {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$CMakeExe)

    $cmakeDir = Split-Path -Parent $CMakeExe
    if ($cmakeDir) {
        $candidate = Join-Path $cmakeDir 'ctest.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    $command = Get-Command ctest.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    return $null
}

function Get-Sha256Hex {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Get-Command Get-FileHash -ErrorAction SilentlyContinue) {
        return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }

    $stream = [System.IO.File]::OpenRead((Resolve-Path $Path).Path)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $bytes = $sha.ComputeHash($stream)
        return [BitConverter]::ToString($bytes).Replace('-', '')
    } finally {
        $stream.Close()
    }
}

function Get-GitCommitHash {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$RepositoryRoot)

    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        return $null
    }

    $commit = & $git.Source -C $RepositoryRoot rev-parse HEAD 2>$null
    if ($LASTEXITCODE -ne 0) {
        return $null
    }

    $trimmed = $commit.Trim()
    if ([string]::IsNullOrWhiteSpace($trimmed)) {
        return $null
    }

    return $trimmed
}

function Resolve-ToolFromPathOrVcpkg {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ToolName,
        [Parameter(Mandatory = $true)][string]$VcpkgRoot
    )

    $command = Get-Command $ToolName -ErrorAction SilentlyContinue
    if ($command -and $command.Source) {
        return $command.Source
    }

    $toolStem = [System.IO.Path]::GetFileNameWithoutExtension($ToolName)
    $searchRoots = @(
        (Join-Path $VcpkgRoot "downloads\tools\$toolStem"),
        (Join-Path $VcpkgRoot "tools\$toolStem"),
        (Join-Path $VcpkgRoot "installed\x64-windows\tools\$toolStem"),
        (Join-Path $VcpkgRoot "installed\x86-windows\tools\$toolStem")
    )

    $matches = @()
    foreach ($root in $searchRoots) {
        if (-not (Test-Path -LiteralPath $root)) {
            continue
        }

        $matches += Get-ChildItem -Path $root -Recurse -File -Filter $ToolName -ErrorAction SilentlyContinue
    }

    if ($matches.Count -eq 0) {
        return $null
    }

    return ($matches | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}

function Get-CMakeGeneratorFromCache {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$BuildDirectory)

    $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cachePath)) {
        return $null
    }

    $line = Get-Content -LiteralPath $cachePath |
        Where-Object { $_ -like 'CMAKE_GENERATOR:INTERNAL=*' } |
        Select-Object -First 1

    if (-not $line) {
        return $null
    }

    return ($line -replace '^CMAKE_GENERATOR:INTERNAL=', '').Trim()
}
