#requires -Version 5.1

# Filesystem, packaging-copy, and build-time runtime/scheduled-task control helpers.
# Extracted from Build-Release.ps1; dot-sourced by it. Functions are
# parameter-driven and invoked from the Build-Release.ps1 pipeline scope.

function Remove-DirectoryIfExists {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    if (Test-Path -LiteralPath $Path) {
        try {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
        } catch {
            Write-Warning ("Could not remove directory: {0} ({1})" -f $Path, $_.Exception.Message)
        }
    }
}

function New-EmptyDirectory {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    Remove-DirectoryIfExists -Path $Path
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Test-PathUnderDirectory {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Directory
    )

    try {
        $fullPath = [System.IO.Path]::GetFullPath($Path)
        $fullDirectory = [System.IO.Path]::GetFullPath($Directory).TrimEnd('\', '/')
        return $fullPath.StartsWith($fullDirectory + '\', [System.StringComparison]::OrdinalIgnoreCase)
    } catch {
        return $false
    }
}

function Test-RelativePathContainsSegment {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string[]]$Segments
    )

    $parts = $Path -split '[\\/]+' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    foreach ($part in $parts) {
        foreach ($segment in $Segments) {
            if ([string]::Equals($part, $segment, [System.StringComparison]::OrdinalIgnoreCase)) {
                return $true
            }
        }
    }
    return $false
}

function Start-PackagedController {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ReleaseRoot,
        [Parameter(Mandatory = $true)][string]$MainExeName
    )

    $exePath = Join-Path $ReleaseRoot $MainExeName
    $configPath = Join-Path $ReleaseRoot 'control.json'
    if (-not (Test-Path -LiteralPath $exePath)) {
        Write-Warning "Packaged controller restart skipped; executable missing: $exePath"
        return $false
    }
    if (-not (Test-Path -LiteralPath $configPath)) {
        Write-Warning "Packaged controller restart skipped; config missing: $configPath"
        return $false
    }

    Write-Host "`n[cleanup] Restarting packaged controller..." -ForegroundColor Yellow
    & $exePath --start --config $configPath
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Packaged controller restart failed with exit code $LASTEXITCODE."
        return $false
    }
    return $true
}

function Suspend-ScheduledTaskIfEnabled {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$TaskName)

    $queryOutput = & schtasks.exe /Query /TN $TaskName /FO LIST 2>$null
    if ($LASTEXITCODE -ne 0) {
        # Task not found under this exact name/path (for example an installer ran
        # with a custom -TaskName/-TaskPath). The build-time suspend is skipped;
        # surface it with -Verbose instead of silently no-op'ing.
        Write-Verbose "Watchdog task not found; skipping build-time suspend: $TaskName"
        return $false
    }
    $wasEnabled = -not (($queryOutput -join "`n") -match '(?im)^\s*Scheduled Task State:\s*Disabled\s*$')
    if ($wasEnabled) {
        & schtasks.exe /Change /TN $TaskName /DISABLE | Out-Null
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Could not disable scheduled task: $TaskName"
            return $false
        }
        & schtasks.exe /End /TN $TaskName 2>$null | Out-Null
    }
    return $wasEnabled
}

function Resume-ScheduledTaskIfNeeded {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$TaskName,
        [Parameter(Mandatory = $true)][bool]$ShouldEnable
    )

    if (-not $ShouldEnable) {
        return
    }
    & schtasks.exe /Change /TN $TaskName /ENABLE | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Could not re-enable scheduled task: $TaskName"
    }
}

function Copy-DistExtra {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$DestRoot,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    $source = Join-Path $SourceRoot $RelativePath
    if (-not (Test-Path -LiteralPath $source)) {
        Write-Warning "Dist extra not found, skipping: $RelativePath"
        return
    }

    $dest = Join-Path $DestRoot $RelativePath
    $destParent = Split-Path -Parent $dest
    if ($destParent -and -not (Test-Path -LiteralPath $destParent)) {
        New-Item -ItemType Directory -Path $destParent -Force | Out-Null
    }

    $item = Get-Item -LiteralPath $source
    if ($item.PSIsContainer) {
        Copy-Item -LiteralPath $source -Destination $dest -Recurse -Force
    } else {
        Copy-Item -LiteralPath $source -Destination $dest -Force
    }

    Write-Host "Copied: $RelativePath" -ForegroundColor Green
}

function Copy-DistFileAs {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    if (-not (Test-Path -LiteralPath $SourcePath)) {
        throw "Dist file not found: $SourcePath"
    }

    $destParent = Split-Path -Parent $DestinationPath
    if ($destParent -and -not (Test-Path -LiteralPath $destParent)) {
        New-Item -ItemType Directory -Path $destParent -Force | Out-Null
    }

    Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Force
    Write-Host "Copied: $(Split-Path -Leaf $SourcePath) -> $(Split-Path -Leaf $DestinationPath)" -ForegroundColor Green
}
