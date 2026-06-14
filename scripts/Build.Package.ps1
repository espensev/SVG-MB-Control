#requires -Version 5.1

# Filesystem, packaging-copy, and build-time runtime/scheduled-task control helpers.
# Extracted from Build-Release.ps1; dot-sourced by it. Functions are
# parameter-driven and invoked from the Build-Release.ps1 pipeline scope.

function Remove-DirectoryIfExists {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        # With -Strict, a removal that fails or leaves the directory behind
        # throws instead of warning. Use it for the pre-build clean so the
        # pipeline fails loud rather than building against stale artifacts; leave
        # it off for post-build cleanup, where a leftover is not worth failing on.
        [switch]$Strict
    )

    if (Test-Path -LiteralPath $Path) {
        try {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
        } catch {
            if ($Strict) {
                throw ("Could not remove directory: {0} ({1})" -f $Path, $_.Exception.Message)
            }
            Write-Warning ("Could not remove directory: {0} ({1})" -f $Path, $_.Exception.Message)
            return
        }
        # Remove-Item can return without error yet leave a locked subtree behind
        # (for example a held .obj from another process), so verify the removal.
        if ($Strict -and (Test-Path -LiteralPath $Path)) {
            throw ("Directory still present after removal (a file is likely locked): {0}" -f $Path)
        }
    }
}

function New-EmptyDirectory {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$Strict
    )

    Remove-DirectoryIfExists -Path $Path -Strict:$Strict
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

function Get-WatchdogSuspendSentinelPath {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    # Lives at the repo root (not under release/) so it is not hashed into
    # build-info.json, survives a reboot, and is already covered by .gitignore.
    Join-Path $RepoRoot '.svg-mb-build-watchdog-suspended'
}

function Set-WatchdogSuspendSentinel {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    # Records that this build disabled the watchdog. If the file is still present
    # when the next build starts, that build was interrupted (terminal closed,
    # reboot) before it could re-enable the watchdog, and the next build heals it.
    $path = Get-WatchdogSuspendSentinelPath -RepoRoot $RepoRoot
    Set-Content -LiteralPath $path -Value "watchdog disabled by build-release pid=$PID" -Encoding ASCII
}

function Clear-WatchdogSuspendSentinel {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$RepoRoot)

    $path = Get-WatchdogSuspendSentinelPath -RepoRoot $RepoRoot
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
}

function Move-FileReplace {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    # Atomically replace $Destination with $Source on the same volume. MoveFileEx
    # with MOVEFILE_REPLACE_EXISTING keeps the destination present throughout the
    # swap on both Windows PowerShell 5.1 and PowerShell 7+, so an interrupted
    # publish never leaves the controller executable missing.
    if (-not ([System.Management.Automation.PSTypeName]'SvgMb.NativeFile').Type) {
        Add-Type -Namespace 'SvgMb' -Name 'NativeFile' -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern bool MoveFileExW(string lpExistingFileName, string lpNewFileName, uint dwFlags);
'@
    }
    $MOVEFILE_REPLACE_EXISTING = 0x1
    $MOVEFILE_WRITE_THROUGH = 0x8
    $flags = [uint32]($MOVEFILE_REPLACE_EXISTING -bor $MOVEFILE_WRITE_THROUGH)
    if (-not [SvgMb.NativeFile]::MoveFileExW($Source, $Destination, $flags)) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Atomic replace failed ('$Source' -> '$Destination'): Win32 error $err"
    }
}

function Publish-DistToRelease {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$DistDir,
        [Parameter(Mandatory = $true)][string]$ReleaseRoot,
        [string[]]$PreserveNames = @('archive', 'runtime')
    )

    # Publish without ever leaving release/ without the controller executables.
    # The previous implementation deleted everything in release/ (except the
    # preserved directories) and only then copied the new build in; an
    # interruption during that window left release/ with no exe, which is what
    # caused the scheduled task to fail with 0x80070002 ("file not found") after
    # the 2026-06-14 reboot. This copy-over-then-prune approach replaces each
    # top-level file with an atomic rename, so the exe is always present.
    if (-not (Test-Path -LiteralPath $ReleaseRoot)) {
        New-Item -ItemType Directory -Path $ReleaseRoot -Force | Out-Null
    }

    $distItems = @(Get-ChildItem -LiteralPath $DistDir -Force)
    $distNames = @($distItems | Select-Object -ExpandProperty Name)

    foreach ($item in $distItems) {
        $target = Join-Path $ReleaseRoot $item.Name
        if ($item.PSIsContainer) {
            # Clean-replace top-level directories so stale nested files do not
            # accumulate. The scheduled-task executables are top-level files, not
            # directories, so this brief per-directory gap never removes the exe.
            if (Test-Path -LiteralPath $target) {
                Remove-Item -LiteralPath $target -Recurse -Force
            }
            Copy-Item -LiteralPath $item.FullName -Destination $target -Recurse -Force
        } else {
            # Stage beside the target (full copy), then swap in one atomic step.
            # The old file stays intact until the staged copy is complete, so an
            # interruption leaves either the old or the new file, never none.
            $staged = "$target.incoming"
            Copy-Item -LiteralPath $item.FullName -Destination $staged -Force
            if (Test-Path -LiteralPath $target) {
                Move-FileReplace -Source $staged -Destination $target
            } else {
                Move-Item -LiteralPath $staged -Destination $target -Force
            }
        }
    }

    # Prune stale top-level entries the current build no longer produces,
    # preserving runtime state and archive history (and cleaning any leftover
    # .incoming staging files from an interrupted prior publish).
    Get-ChildItem -LiteralPath $ReleaseRoot -Force | Where-Object {
        ($PreserveNames -notcontains $_.Name) -and ($distNames -notcontains $_.Name)
    } | Remove-Item -Recurse -Force
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
