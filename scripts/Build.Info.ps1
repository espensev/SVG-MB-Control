#requires -Version 5.1

# Project version, build-info, version table, and release-archive helpers.
# Extracted from Build-Release.ps1; dot-sourced by it. Functions are
# parameter-driven and invoked from the Build-Release.ps1 pipeline scope.

function Get-ProjectVersion {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$VersionFilePath)

    if (-not (Test-Path -LiteralPath $VersionFilePath)) {
        throw "VERSION file not found at: $VersionFilePath"
    }

    $raw = (Get-Content -LiteralPath $VersionFilePath -Raw).Trim()
    if ($raw -notmatch '^\d+\.\d+\.\d+$') {
        throw "Invalid version format in VERSION file: '$raw' (expected major.minor.patch)"
    }

    return $raw
}

function New-BuildInfo {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ArtifactRoot,
        [Parameter(Mandatory = $true)][string]$MainArtifactPath,
        [Parameter(Mandatory = $true)][string]$ProjectName,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$Architecture,
        [Parameter(Mandatory = $true)][string]$PresetName,
        [Parameter(Mandatory = $true)][bool]$TestsRun,
        [Parameter(Mandatory = $true)][bool]$TestsPassed,
        [string]$SourceCommit,
        [bool]$WorkingTreeDirty = $false
    )

    $mainFile = Get-Item -LiteralPath $MainArtifactPath
    $mainHash = Get-Sha256Hex -Path $MainArtifactPath
    $artifactRootFull = (Resolve-Path -LiteralPath $ArtifactRoot).ProviderPath.TrimEnd('\','/')
    $artifactHashes = @(
        Get-ChildItem -LiteralPath $ArtifactRoot -File -Recurse |
            Sort-Object FullName |
            ForEach-Object {
                $relative = $_.FullName
                if ($relative.StartsWith($artifactRootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
                    $relative = $relative.Substring($artifactRootFull.Length).TrimStart('\','/')
                }
                [ordered]@{
                    path   = $relative
                    size   = $_.Length
                    sha256 = Get-Sha256Hex -Path $_.FullName
                }
            }
    )

    $info = [ordered]@{
        project        = $ProjectName
        version        = $Version
        mainExe        = $mainFile.Name
        sha256         = $mainHash
        size           = $mainFile.Length
        architecture   = $Architecture
        preset         = $PresetName
        builtUtc       = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        testsRun         = $TestsRun
        testsPassed      = $TestsPassed
        workingTreeDirty = $WorkingTreeDirty
        artifactCount    = $artifactHashes.Count
        artifactHashes   = $artifactHashes
    }

    if ($SourceCommit) {
        # sourceCommit stays a pure git ref (for checkout/reproduction); the
        # workingTreeDirty flag (and the -dirty version suffix) carry dirtiness.
        $info['sourceCommit'] = $SourceCommit
    }

    $outPath = Join-Path $ArtifactRoot 'build-info.json'
    $info | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $outPath -Encoding UTF8
    return $outPath
}

function Update-BuildInfoArchive {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$BuildInfoPath,
        [Parameter(Mandatory = $true)][string]$ArchivePath
    )

    $buildInfo = Get-Content -LiteralPath $BuildInfoPath -Raw | ConvertFrom-Json
    $archiveName = Split-Path -Path $ArchivePath -Leaf

    if ($buildInfo.PSObject.Properties.Name -contains 'archive') {
        $buildInfo.archive = $archiveName
    } else {
        $buildInfo | Add-Member -NotePropertyName archive -NotePropertyValue $archiveName
    }

    $buildInfo | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $BuildInfoPath -Encoding UTF8
    return $BuildInfoPath
}

function Write-VersionTable {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ReleaseDirPath,
        [Parameter(Mandatory = $true)][string]$ArchiveDirectory,
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$ProjectName
    )

    $tablePath = Join-Path $ReleaseDirPath 'VERSION_TABLE.json'
    $persistentPath = Join-Path $ArchiveDirectory 'VERSION_TABLE.json'
    $existingPath = if (Test-Path -LiteralPath $persistentPath) {
        $persistentPath
    } elseif (Test-Path -LiteralPath $tablePath) {
        $tablePath
    } else {
        $null
    }

    $entries = @()
    if ($existingPath) {
        try {
            $existing = Get-Content -LiteralPath $existingPath -Raw | ConvertFrom-Json
            if ($existing.builds) {
                $entries = @($existing.builds)
            }
        } catch {
            Write-Warning 'Could not parse existing VERSION_TABLE.json, starting fresh.'
        }
    }

    $buildInfoPath = Join-Path $ReleaseDirPath 'build-info.json'
    $buildInfo = Get-Content -LiteralPath $buildInfoPath -Raw | ConvertFrom-Json

    $newEntry = [ordered]@{
        version  = $Version
        sha256   = $buildInfo.sha256
        size     = $buildInfo.size
        builtUtc = $buildInfo.builtUtc
        archive  = Split-Path -Path $ArchivePath -Leaf
    }

    $replaced = $false
    $updatedEntries = @()
    foreach ($entry in $entries) {
        if ($entry.version -eq $Version) {
            $updatedEntries += $newEntry
            $replaced = $true
        } else {
            $updatedEntries += $entry
        }
    }

    if (-not $replaced) {
        $updatedEntries += $newEntry
    }

    $updatedEntries = @($updatedEntries | Sort-Object builtUtc -Descending)

    $table = [ordered]@{
        project        = $ProjectName
        generatedAtUtc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        builds         = @($updatedEntries)
    }

    $table | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $persistentPath -Encoding UTF8
    Copy-Item -LiteralPath $persistentPath -Destination $tablePath -Force
    return $tablePath
}

function New-ReleaseArchive {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ReleaseDirPath,
        [Parameter(Mandatory = $true)][string]$ArchiveDirectory,
        [Parameter(Mandatory = $true)][string]$ProjectName,
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)][string[]]$SourceGlobs
    )

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    if (-not (Test-Path -LiteralPath $ArchiveDirectory)) {
        New-Item -ItemType Directory -Path $ArchiveDirectory -Force | Out-Null
    }

    $timestamp = (Get-Date).ToString('yyyyMMdd-HHmm')
    $zipName = "$ProjectName-$timestamp.zip"
    $zipPath = Join-Path $ArchiveDirectory $zipName
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }

    $zip = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        $prefix = "$ProjectName-$timestamp"
        $seenEntries = @{}

        $releaseRootFull = (Resolve-Path -LiteralPath $ReleaseDirPath).ProviderPath.TrimEnd('\','/')
        $releaseFiles = Get-ChildItem -LiteralPath $ReleaseDirPath -File -Recurse | Sort-Object FullName
        foreach ($file in $releaseFiles) {
            $relativePath = $file.FullName
            if ($relativePath.StartsWith($releaseRootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
                $relativePath = $relativePath.Substring($releaseRootFull.Length).TrimStart('\','/')
            }
            $relativePath = $relativePath -replace '\\', '/'
            if ($relativePath -like 'archive/*' -or
                (Test-RelativePathContainsSegment -Path $relativePath -Segments @('runtime'))) {
                continue
            }

            $entryName = "$prefix/$relativePath"
            if ($seenEntries.ContainsKey($entryName)) { continue }
            $seenEntries[$entryName] = $true
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $file.FullName, $entryName,
                [System.IO.Compression.CompressionLevel]::Optimal
            ) | Out-Null
        }

        $excludeDirs = @('build', 'dist', 'release', 'logs', '.vs', '.git', 'runtime', '.pytest_cache', '__pycache__')
        foreach ($glob in $SourceGlobs) {
            $matches = Get-ChildItem -Path $ProjectRoot -Filter $glob -Recurse -File -ErrorAction SilentlyContinue |
                Sort-Object FullName
            foreach ($file in $matches) {
                $relativePath = $file.FullName.Substring($ProjectRoot.Length).TrimStart([char[]]@('\', '/'))
                if (Test-RelativePathContainsSegment -Path $relativePath -Segments $excludeDirs) { continue }

                $entryName = "$prefix/src/$($relativePath -replace '\\', '/')"
                if ($seenEntries.ContainsKey($entryName)) { continue }
                $seenEntries[$entryName] = $true
                [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $zip, $file.FullName, $entryName,
                    [System.IO.Compression.CompressionLevel]::Optimal
                ) | Out-Null
            }
        }
    } finally {
        $zip.Dispose()
    }

    return $zipPath
}

function Sync-ReleaseMetadataIntoArchive {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ArchivePath,
        [Parameter(Mandatory = $true)][string]$ReleaseDirPath,
        [string[]]$FileNames = @('build-info.json', 'VERSION_TABLE.json')
    )

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $archivePrefix = [System.IO.Path]::GetFileNameWithoutExtension($ArchivePath)
    $zip = [System.IO.Compression.ZipFile]::Open($ArchivePath, [System.IO.Compression.ZipArchiveMode]::Update)
    try {
        foreach ($name in $FileNames) {
            $sourcePath = Join-Path $ReleaseDirPath $name
            if (-not (Test-Path -LiteralPath $sourcePath)) {
                continue
            }

            $entryName = "$archivePrefix/$name"
            $existing = $zip.GetEntry($entryName)
            if ($existing) {
                $existing.Delete()
            }

            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, $sourcePath, $entryName,
                [System.IO.Compression.CompressionLevel]::Optimal
            ) | Out-Null
        }
    } finally {
        $zip.Dispose()
    }
}
