# build-release.ps1 — CMake Release build, test, package, publish, and archive
# Usage:  .\build-release.ps1 [-KeepBuildDir] [-SkipTests] [-Verbose] [-Help]

[CmdletBinding()]
param(
    [switch]$KeepBuildDir,
    [ValidateSet('x64')]
    [string]$Architecture = 'x64',
    [switch]$SkipTests,
    [Alias('h')][switch]$Help
)

if ($Help) {
    Write-Host @"
build-release.ps1 — CMake Release build, test, package, publish, and archive

USAGE
    .\build-release.ps1 [options]

OPTIONS
    -Architecture   Target architecture preset (default: x64)
    -KeepBuildDir   Keep the build/ directory after completion (default: removed)
    -SkipTests      Skip python -m unittest discover tests -v
    -Verbose        Show verbose/diagnostic output
    -Help, -h       Show this help message and exit

PIPELINE
    0  Stop running processes      6  Package to dist/
    1  Read repo version           7  Verify artifacts
    2  Clean build directories     8  Run hermetic tests
    3  Init VS environment         9  Publish to release/
    4  Validate prerequisites     10  Archive + version table
    5  CMake configure + build    11  Cleanup

OUTPUT
    release/             Latest build (exe + helper + build-info.json + docs/config)
    release/archive/     Timestamped zip archives

NOTES
    VCPKG_ROOT must point to a vcpkg checkout. The script resolves the
    vcpkg toolchain plus cmake.exe and ninja.exe from there when needed.
"@
    return
}

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ── Project Configuration ────────────────────────────────────────────
$ProjectName         = 'svg-mb-control'
$MainExeName         = "$ProjectName.exe"
$SupportExeNames     = @()
$ProcessNames        = @('svg-mb-control', 'fake-bench')
$ReleaseDir          = 'release'
$DistExtras          = @(
    'README.md'
    'CLAUDE.md'
    'docs'
    'config\control.example.json'
    'VERSION'
)
$SourceGlobs         = @(
    '*.cpp', '*.cc', '*.cxx',
    '*.h', '*.hpp', '*.hh', '*.inl',
    '*.ps1', '*.py',
    '*.md', '*.json',
    '*.cmake', '*.rc', '*.rc.in',
    'CMakeLists.txt', 'CMakePresets.json',
    'VERSION'
)
# ─────────────────────────────────────────────────────────────────────

# Derived paths
$RepoRoot            = if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'CMakeLists.txt')) { $PSScriptRoot } else { Split-Path -Parent $PSScriptRoot }
$BuildRoot           = Join-Path $RepoRoot 'build'
$PresetName          = "$Architecture-release"
$BuildDir            = Join-Path $BuildRoot $PresetName
$DistDir             = Join-Path $RepoRoot 'dist'
$ReleaseRoot         = Join-Path $RepoRoot $ReleaseDir
$ArchiveDir          = Join-Path $ReleaseRoot 'archive'
$VersionFile         = Join-Path $RepoRoot 'VERSION'
$configuredVcpkgRoot = $env:VCPKG_ROOT

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

function Get-VsWherePath {
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return $candidate
        }
    }

    return $null
}

function Get-VsInstallPath {
    $vswhere = Get-VsWherePath
    if (-not $vswhere) {
        return $null
    }

    $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($installPath) {
        return $installPath.Trim()
    }

    return $null
}

function Get-VsInstanceId {
    $vswhere = Get-VsWherePath
    if (-not $vswhere) {
        return $null
    }

    $instanceId = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property instanceId 2>$null
    if ($instanceId) {
        return $instanceId.Trim()
    }

    return $null
}

function Resolve-VsDevCmdPath {
    $installPath = Get-VsInstallPath
    if ($installPath) {
        $candidate = Join-Path -Path $installPath -ChildPath 'Common7\Tools\VsDevCmd.bat'
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    $programRoots = @(${env:ProgramFiles(x86)}, $env:ProgramFiles)
    $versions = @('18', '2022', '2019')
    $editions = @('BuildTools', 'Enterprise', 'Professional', 'Community')
    foreach ($root in $programRoots) {
        if (-not $root) { continue }
        foreach ($ver in $versions) {
            foreach ($ed in $editions) {
                $candidate = Join-Path $root "Microsoft Visual Studio\$ver\$ed\Common7\Tools\VsDevCmd.bat"
                if (Test-Path -LiteralPath $candidate) {
                    return $candidate
                }
            }
        }
    }

    return $null
}

function Import-VsDevCmdEnvironment {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$VsDevCmdPath,
        [string]$Arch = 'amd64',
        [string]$HostArch = 'amd64'
    )

    $cmd = "call `"$VsDevCmdPath`" -arch=$Arch -host_arch=$HostArch >nul && set"
    $lines = & cmd.exe /d /s /c $cmd
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to initialize VS environment via VsDevCmd.bat (exit code: $LASTEXITCODE)."
    }

    foreach ($line in $lines) {
        if (-not $line) { continue }
        $idx = $line.IndexOf('=')
        if ($idx -lt 1) { continue }

        $name = $line.Substring(0, $idx)
        if ($name.StartsWith('=')) { continue }

        $value = $line.Substring($idx + 1)
        [System.Environment]::SetEnvironmentVariable($name, $value, [System.EnvironmentVariableTarget]::Process)
    }
}

function Import-VsDevShellEnvironment {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$VsInstallPath,
        [string]$VsInstanceId,
        [string]$Arch = 'amd64',
        [string]$HostArch = 'amd64'
    )

    $devShellDll = Join-Path -Path $VsInstallPath -ChildPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    if (-not (Test-Path -LiteralPath $devShellDll)) {
        throw "Visual Studio DevShell module not found at: $devShellDll"
    }

    Import-Module $devShellDll -ErrorAction Stop

    if ($VsInstanceId) {
        Enter-VsDevShell -VsInstanceId $VsInstanceId -SkipAutomaticLocation -Arch $Arch -HostArch $HostArch -ErrorAction Stop | Out-Null
    } else {
        Enter-VsDevShell -VsInstallPath $VsInstallPath -SkipAutomaticLocation -Arch $Arch -HostArch $HostArch -ErrorAction Stop | Out-Null
    }
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

function Resolve-PythonRunner {
    [CmdletBinding()]
    param()

    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python -and $python.Source) {
        return @{
            FilePath   = $python.Source
            PrefixArgs = @()
        }
    }

    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py -and $py.Source) {
        return @{
            FilePath   = $py.Source
            PrefixArgs = @('-3')
        }
    }

    return $null
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
        [string]$SourceCommit
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
        testsRun       = $TestsRun
        testsPassed    = $TestsPassed
        artifactCount  = $artifactHashes.Count
        artifactHashes = $artifactHashes
    }

    if ($SourceCommit) {
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
            if ($relativePath -like 'archive/*') {
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
                $skip = $false
                foreach ($excludeDir in $excludeDirs) {
                    if ($relativePath -like "$excludeDir\\*" -or $relativePath -like "$excludeDir/*") {
                        $skip = $true
                        break
                    }
                }
                if ($skip) { continue }

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

function Invoke-HermeticTests {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$RepositoryRoot)

    $pythonRunner = Resolve-PythonRunner
    if (-not $pythonRunner) {
        throw 'Python 3 was not found. Install python or rerun with -SkipTests.'
    }

    $arguments = @($pythonRunner['PrefixArgs']) + @('-m', 'unittest', 'discover', 'tests', '-v')
    Push-Location -LiteralPath $RepositoryRoot
    try {
        Invoke-External -FilePath $pythonRunner['FilePath'] -Arguments $arguments -FailureMessage 'Hermetic test lane failed'
    } finally {
        Pop-Location
    }
}

# ── Build Pipeline ───────────────────────────────────────────────────

$timer = [System.Diagnostics.Stopwatch]::StartNew()
$buildSucceeded = $false
$testsRun = -not $SkipTests
$testsPassed = $false
$sourceCommit = $null
$version = $null
$buildInfoPath = $null
$zipPath = $null
$zipSize = 0
$mainExeHash = $null

Write-Host "--- Build pipeline: $ProjectName (Release) ---" -ForegroundColor Cyan
Write-Host "Architecture: $Architecture"
Write-Host "Preset      : $PresetName"
Write-Host "Project root: $RepoRoot"
Write-Host "Build dir   : $BuildDir"
Write-Host "Dist dir    : $DistDir"
Write-Host "Release dir : $ReleaseRoot"
Write-Host "Archive dir : $ArchiveDir"

try {
    Write-Host "`n[0/11] Stopping running processes..." -ForegroundColor Yellow
    foreach ($processName in $ProcessNames) {
        $proc = Get-Process -Name $processName -ErrorAction SilentlyContinue
        if ($proc) {
            $proc | Stop-Process -Force
            Write-Host "Stopped: $processName" -ForegroundColor Green
        } else {
            Write-Host "No running process found for $processName."
        }
    }

    Write-Host "`n[1/11] Reading repo version..." -ForegroundColor Yellow
    $version = Get-ProjectVersion -VersionFilePath $VersionFile
    Write-Host "Version: $version" -ForegroundColor Green

    Write-Host "`n[2/11] Cleaning build directories..." -ForegroundColor Yellow
    Remove-DirectoryIfExists -Path $BuildRoot
    New-EmptyDirectory -Path $BuildDir
    New-EmptyDirectory -Path $DistDir

    Write-Host "`n[3/11] Initializing Visual Studio environment..." -ForegroundColor Yellow
    $vsInstallPath = Get-VsInstallPath
    $vsInstanceId = Get-VsInstanceId
    $devShellLoaded = $false

    if ($vsInstallPath) {
        try {
            Import-VsDevShellEnvironment -VsInstallPath $vsInstallPath -VsInstanceId $vsInstanceId -Arch amd64 -HostArch amd64
            $devShellLoaded = $true
            Write-Host "VS environment loaded via DevShell: $vsInstallPath" -ForegroundColor Green
        } catch {
            Write-Verbose "DevShell failed, falling back to VsDevCmd: $($_.Exception.Message)"
        }
    }

    if (-not $devShellLoaded) {
        $vsDevCmd = Resolve-VsDevCmdPath
        if (-not $vsDevCmd) {
            throw 'VsDevCmd.bat not found. Install Visual Studio with C++ build tools.'
        }

        Import-VsDevCmdEnvironment -VsDevCmdPath $vsDevCmd -Arch amd64 -HostArch amd64
        Write-Host "VS environment loaded via VsDevCmd.bat: $vsDevCmd" -ForegroundColor Green
    }

    if (-not [string]::IsNullOrWhiteSpace($configuredVcpkgRoot)) {
        $env:VCPKG_ROOT = $configuredVcpkgRoot
    }

    Write-Host "`n[4/11] Validating prerequisites..." -ForegroundColor Yellow
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'MSVC compiler (cl.exe) not found after Visual Studio environment initialization.'
    }

    $vcpkgRoot = $configuredVcpkgRoot
    if ([string]::IsNullOrWhiteSpace($vcpkgRoot)) {
        throw 'VCPKG_ROOT is not set.'
    }
    $vcpkgRoot = [System.IO.Path]::GetFullPath($vcpkgRoot)
    if (-not (Test-Path -LiteralPath $vcpkgRoot)) {
        throw "VCPKG_ROOT path does not exist: $vcpkgRoot"
    }

    $vcpkgToolchain = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
    if (-not (Test-Path -LiteralPath $vcpkgToolchain)) {
        throw "vcpkg toolchain not found: $vcpkgToolchain"
    }

    $triplet = 'x64-windows'
    $cmakeExe = Resolve-ToolFromPathOrVcpkg -ToolName 'cmake.exe' -VcpkgRoot $vcpkgRoot
    if (-not $cmakeExe) {
        throw 'cmake not found in PATH or VCPKG_ROOT tools.'
    }

    $ninjaExe = Resolve-ToolFromPathOrVcpkg -ToolName 'ninja.exe' -VcpkgRoot $vcpkgRoot
    if (-not $ninjaExe) {
        throw 'ninja not found in PATH or VCPKG_ROOT tools.'
    }

    $cmakeLists = Join-Path $RepoRoot 'CMakeLists.txt'
    if (-not (Test-Path -LiteralPath $cmakeLists)) {
        throw "CMakeLists.txt not found at: $cmakeLists"
    }

    $cmakePresets = Join-Path $RepoRoot 'CMakePresets.json'
    if (-not (Test-Path -LiteralPath $cmakePresets)) {
        throw "CMakePresets.json not found at: $cmakePresets"
    }

    $env:CMAKE_GENERATOR = 'Ninja'
    $env:VCPKG_ROOT = $vcpkgRoot
    $sourceCommit = Get-GitCommitHash -RepositoryRoot $RepoRoot

    Write-Host "cl.exe : $(Get-Command cl.exe | Select-Object -ExpandProperty Source)" -ForegroundColor Green
    Write-Host "cmake  : $cmakeExe" -ForegroundColor Green
    Write-Host "ninja  : $ninjaExe" -ForegroundColor Green
    Write-Host "vcpkg  : $vcpkgRoot" -ForegroundColor Green
    Write-Host "triplet: $triplet" -ForegroundColor Green
    if ($sourceCommit) {
        Write-Host "commit : $sourceCommit" -ForegroundColor Green
    } else {
        Write-Host "commit : unavailable" -ForegroundColor DarkGray
    }

    Write-Host "`n[5/11] CMake configure..." -ForegroundColor Yellow
    $configureArgs = @(
        '--preset', $PresetName,
        "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchain",
        "-DVCPKG_TARGET_TRIPLET=$triplet",
        "-DCMAKE_MAKE_PROGRAM=$ninjaExe",
        '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_GENERATOR=Ninja'
    )

    Push-Location -LiteralPath $RepoRoot
    try {
        Invoke-External -FilePath $cmakeExe -Arguments $configureArgs -FailureMessage 'CMake configure failed'
    } finally {
        Pop-Location
    }

    $cachePath = Join-Path $BuildDir 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cachePath)) {
        throw "Expected CMake cache not found at $cachePath after preset '$PresetName'."
    }

    $configuredGenerator = Get-CMakeGeneratorFromCache -BuildDirectory $BuildDir
    if ($configuredGenerator -and $configuredGenerator -ne 'Ninja') {
        throw "Preset '$PresetName' configured generator '$configuredGenerator', expected 'Ninja'."
    }

    Write-Host "`n[5b/11] CMake build..." -ForegroundColor Yellow
    Invoke-External -FilePath $cmakeExe -Arguments @(
        '--build', '--preset', $PresetName, '--parallel'
    ) -FailureMessage 'CMake build failed'

    Write-Host "`n[6/11] Packaging to dist/..." -ForegroundColor Yellow
    $builtMainExe = Join-Path $BuildDir $MainExeName
    if (-not (Test-Path -LiteralPath $builtMainExe)) {
        $builtMainExe = Join-Path $BuildDir "Release\$MainExeName"
    }
    if (-not (Test-Path -LiteralPath $builtMainExe)) {
        throw "Build completed but $MainExeName was not found under $BuildDir."
    }

    $distMainExe = Join-Path $DistDir $MainExeName
    Copy-Item -LiteralPath $builtMainExe -Destination $distMainExe -Force
    Write-Host "Copied: $MainExeName" -ForegroundColor Green

    foreach ($supportExeName in $SupportExeNames) {
        $builtSupportExe = Join-Path $BuildDir $supportExeName
        if (-not (Test-Path -LiteralPath $builtSupportExe)) {
            $builtSupportExe = Join-Path $BuildDir "Release\$supportExeName"
        }
        if (-not (Test-Path -LiteralPath $builtSupportExe)) {
            throw "Build completed but $supportExeName was not found under $BuildDir."
        }

        Copy-Item -LiteralPath $builtSupportExe -Destination (Join-Path $DistDir $supportExeName) -Force
        Write-Host "Copied: $supportExeName" -ForegroundColor Green
    }

    $runtimeDlls = Get-ChildItem -LiteralPath (Split-Path -Parent $builtMainExe) -File -Filter '*.dll' -ErrorAction SilentlyContinue |
        Sort-Object Name
    foreach ($dll in $runtimeDlls) {
        Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $DistDir $dll.Name) -Force
        Write-Host "Copied: $($dll.Name)" -ForegroundColor Green
    }

    foreach ($extra in $DistExtras) {
        Copy-DistExtra -SourceRoot $RepoRoot -DestRoot $DistDir -RelativePath $extra
    }
    Copy-DistFileAs `
        -SourcePath (Join-Path $RepoRoot 'config\control.release.json') `
        -DestinationPath (Join-Path $DistDir 'control.json')

    Write-Host "`n[7/11] Verifying artifacts..." -ForegroundColor Yellow
    $mainBuiltHash = Get-Sha256Hex -Path $builtMainExe
    $mainExeHash = Get-Sha256Hex -Path $distMainExe
    if ($mainBuiltHash -ne $mainExeHash) {
        throw "Hash mismatch between built exe and dist copy. built=$mainBuiltHash dist=$mainExeHash"
    }

    foreach ($supportExeName in $SupportExeNames) {
        $builtSupportExe = Join-Path $BuildDir $supportExeName
        if (-not (Test-Path -LiteralPath $builtSupportExe)) {
            $builtSupportExe = Join-Path $BuildDir "Release\$supportExeName"
        }

        $distSupportExe = Join-Path $DistDir $supportExeName
        $builtSupportHash = Get-Sha256Hex -Path $builtSupportExe
        $distSupportHash = Get-Sha256Hex -Path $distSupportExe
        if ($builtSupportHash -ne $distSupportHash) {
            throw "Hash mismatch between built and packaged $supportExeName. built=$builtSupportHash dist=$distSupportHash"
        }
    }

    Write-Host "Main exe SHA256: $mainExeHash" -ForegroundColor Green
    Write-Host ("Main exe size  : {0:N0} bytes" -f (Get-Item -LiteralPath $distMainExe).Length)

    if ($SkipTests) {
        Write-Host "`n[8/11] Hermetic tests skipped." -ForegroundColor DarkGray
    } else {
        Write-Host "`n[8/11] Running hermetic tests..." -ForegroundColor Yellow
        Invoke-HermeticTests -RepositoryRoot $RepoRoot
        $testsPassed = $true
        Write-Host "Hermetic test lane passed." -ForegroundColor Green
    }

    Write-Host "`n[9/11] Publishing to release/..." -ForegroundColor Yellow
    if (Test-Path -LiteralPath $ReleaseRoot) {
        Get-ChildItem -LiteralPath $ReleaseRoot -Force | Where-Object {
            -not ($_.PSIsContainer -and $_.Name -eq 'archive')
        } | Remove-Item -Recurse -Force
    } else {
        New-Item -ItemType Directory -Path $ReleaseRoot -Force | Out-Null
    }

    Copy-Item -Path (Join-Path $DistDir '*') -Destination $ReleaseRoot -Recurse -Force
    Write-Host "Copied dist/ contents to release/" -ForegroundColor Green

    $buildInfoPath = New-BuildInfo `
        -ArtifactRoot $ReleaseRoot `
        -MainArtifactPath (Join-Path $ReleaseRoot $MainExeName) `
        -ProjectName $ProjectName `
        -Version $version `
        -Architecture $Architecture `
        -PresetName $PresetName `
        -TestsRun $testsRun `
        -TestsPassed $testsPassed `
        -SourceCommit $sourceCommit
    Write-Host 'Wrote: build-info.json' -ForegroundColor Green

    Write-Host "`n[10/11] Creating release archive..." -ForegroundColor Yellow
    $zipPath = New-ReleaseArchive `
        -ReleaseDirPath $ReleaseRoot `
        -ArchiveDirectory $ArchiveDir `
        -ProjectName $ProjectName `
        -ProjectRoot $RepoRoot `
        -SourceGlobs $SourceGlobs

    $zipSize = (Get-Item -LiteralPath $zipPath).Length
    $buildInfoPath = Update-BuildInfoArchive -BuildInfoPath $buildInfoPath -ArchivePath $zipPath
    $versionTablePath = Write-VersionTable `
        -ReleaseDirPath $ReleaseRoot `
        -ArchiveDirectory $ArchiveDir `
        -ArchivePath $zipPath `
        -Version $version `
        -ProjectName $ProjectName
    Sync-ReleaseMetadataIntoArchive -ArchivePath $zipPath -ReleaseDirPath $ReleaseRoot

    Write-Host ("Archive: {0}" -f (Split-Path -Path $zipPath -Leaf)) -ForegroundColor Green
    Write-Host ("Size   : {0:N0} bytes" -f $zipSize)
    Write-Host ("Wrote  : {0}" -f (Split-Path -Path $versionTablePath -Leaf)) -ForegroundColor Green

    $buildSucceeded = $true
}
finally {
    Write-Host "`n[11/11] Cleanup..." -ForegroundColor Yellow
    if (Test-Path -LiteralPath $DistDir) {
        Remove-DirectoryIfExists -Path $DistDir
        Write-Host 'Dist directory removed.'
    } else {
        Write-Host 'Dist directory already absent.'
    }

    if ($buildSucceeded) {
        if ($KeepBuildDir) {
            Write-Host "Build directory kept: $BuildDir"
        } else {
            Remove-DirectoryIfExists -Path $BuildRoot
            Write-Host "Build directory removed: $BuildRoot"
        }
    } elseif (Test-Path -LiteralPath $BuildDir) {
        Write-Warning "Build did not complete successfully; keeping build directory at $BuildDir for inspection."
    }
}

$timer.Stop()
$elapsed = $timer.Elapsed

Write-Host "`n--- SUCCESS: $ProjectName v$version ---" -ForegroundColor Green
Write-Host "Release dir : $ReleaseRoot"
if ($sourceCommit) {
    Write-Host "Commit      : $sourceCommit"
}
Get-ChildItem -LiteralPath $ReleaseRoot -File | Format-Table Name, @{Label='Size'; Expression={'{0:N0} bytes' -f $_.Length}} -AutoSize
Write-Host "SHA256      : $mainExeHash"
if ($testsRun) {
    Write-Host "Tests       : passed"
} else {
    Write-Host "Tests       : skipped"
}
Write-Host ("Archive     : {0} ({1:N0} bytes)" -f (Split-Path -Path $zipPath -Leaf), $zipSize)
Write-Host ("Build completed in {0:mm\:ss\.fff}" -f $elapsed)
