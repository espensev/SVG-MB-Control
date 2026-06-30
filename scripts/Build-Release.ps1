# build-release.ps1 — CMake Release build, test, package, publish, and archive
# Usage:  .\build-release.ps1 [-KeepBuildDir] [-SkipTests] [-NoStopProcesses] [-NoPublish] [-Verbose] [-Help]

[CmdletBinding()]
param(
    [switch]$KeepBuildDir,
    [ValidateSet('x64')]
    [string]$Architecture = 'x64',
    [switch]$SkipTests,
    [switch]$NoStopProcesses,
    [switch]$NoPublish,
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
    -SkipTests      Skip CTest and python -m unittest discover tests -v
    -NoStopProcesses
                    Do not stop running svg-mb-control processes before build
    -NoPublish      Build, package to dist/, and test, but do not update
                    release/ or create a release archive
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
    release/             Latest build (exe + build-info.json + docs/config)
    release/archive/     Timestamped zip archives
    release/runtime/     Existing runtime state/logs are preserved on publish

NOTES
    VCPKG_ROOT must point to a vcpkg checkout. The script resolves the
    vcpkg toolchain plus cmake.exe and ninja.exe from there when needed.
    If step 0 stops a controller running from release/, cleanup restarts the
    packaged controller through release/control.json.
    Use -NoPublish with -NoStopProcesses for local validation while a packaged
    controller is running from release/.
"@
    return
}

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ── Project Configuration ────────────────────────────────────────────
$ProjectName         = 'svg-mb-control'
$MainExeName         = "$ProjectName.exe"
$SupportExeNames     = @('svg-mb-control-task-runner.exe')
$ProcessNames        = @('svg-mb-control', 'svg-mb-control-task-runner')
$ReleaseDir          = 'release'
$DistExtras          = @(
    'README.md'
    'CLAUDE.md'
    'Install-SVG-MB-ControlCommon.ps1'
    'Install-SVG-MB-ControlShortcut.ps1'
    'Install-SVG-MB-ControlScheduledTask.ps1'
    'Install-SVG-MB-ControlWatchdogScheduledTask.ps1'
    'Set-SVG-MB-ControlRuntimeWindow.ps1'
    'docs'
    'scripts\Start-EvalDashboard.ps1'
    'scripts\Common-Python.ps1'
    'scripts\Set-EnergyLoggingProfile.ps1'
    'scripts\Compare-CpuTemps.ps1'
    'scripts\Install-CpuTempBaselineTask.ps1'
    'scripts\analyze_cpu_temp_power.py'
    'scripts\analyze_power_lead.py'
    'scripts\control_csv.py'
    'tools\eval_dashboard'
    'config\control.example.json'
    'config\machines'
    'config\runtime_policy_write_live.json'
    'resources'
    'VERSION'
)
$SourceGlobs         = @(
    '*.bin',
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

# Shared Python interpreter resolver, also used by scripts/Start-EvalDashboard.ps1.
. (Join-Path $PSScriptRoot 'Common-Python.ps1')
# Build helper modules (dot-sourced; build-host only, not packaged).
. (Join-Path $PSScriptRoot 'Build.VsEnv.ps1')
. (Join-Path $PSScriptRoot 'Build.Tools.ps1')
. (Join-Path $PSScriptRoot 'Build.Package.ps1')
. (Join-Path $PSScriptRoot 'Build.Info.ps1')
. (Join-Path $PSScriptRoot 'Build.Tests.ps1')

# ── Build Pipeline ───────────────────────────────────────────────────

$timer = [System.Diagnostics.Stopwatch]::StartNew()
$buildSucceeded = $false
$testsRun = -not $SkipTests
$testsPassed = $false
$sourceCommit = $null
$workingTreeDirty = $false
$version = $null
$buildInfoPath = $null
$zipPath = $null
$zipSize = 0
$mainExeHash = $null
$restartPackagedControllerAfterBuild = $false
# Mirrors the default task path + watchdog name from
# Install-SVG-MB-ControlCommon.ps1 ($SvgMbControlTaskPath + $SvgMbWatchdogTaskName);
# keep in sync. Suspend is skipped (with a -Verbose note) when the watchdog was
# installed under a custom name/path.
$watchdogTaskName = '\SVG-MB Control\SVG-MB Control Watchdog'
$resumeWatchdogAfterBuild = $false

Write-Host "--- Build pipeline: $ProjectName (Release) ---" -ForegroundColor Cyan
Write-Host "Architecture: $Architecture"
Write-Host "Preset      : $PresetName"
Write-Host "Project root: $RepoRoot"
Write-Host "Build dir   : $BuildDir"
Write-Host "Dist dir    : $DistDir"
Write-Host "Release dir : $ReleaseRoot"
Write-Host "Archive dir : $ArchiveDir"

# Self-heal: a previous build that was interrupted (terminal closed, reboot)
# after disabling the watchdog leaves the suspend sentinel behind. Because
# Suspend-ScheduledTaskIfEnabled returns $false when the watchdog is already
# disabled, a later build would otherwise never re-enable it, leaving
# boot-resilience off indefinitely. Re-enable it here before this build manages
# it. A watchdog disabled by the operator (no sentinel) is left untouched.
if (Test-Path -LiteralPath (Get-WatchdogSuspendSentinelPath -RepoRoot $RepoRoot)) {
    Write-Host "`nFound watchdog-suspend sentinel from an interrupted build; re-enabling watchdog." -ForegroundColor Yellow
    Resume-ScheduledTaskIfNeeded -TaskName $watchdogTaskName -ShouldEnable $true
    Clear-WatchdogSuspendSentinel -RepoRoot $RepoRoot
}

try {
    if ($NoStopProcesses) {
        Write-Host "`n[0/11] Skipping process stop (-NoStopProcesses)." -ForegroundColor Yellow
    } else {
        Write-Host "`n[0/11] Stopping running processes..." -ForegroundColor Yellow
        $resumeWatchdogAfterBuild = Suspend-ScheduledTaskIfEnabled -TaskName $watchdogTaskName
        if ($resumeWatchdogAfterBuild) {
            Set-WatchdogSuspendSentinel -RepoRoot $RepoRoot
            Write-Host "Temporarily disabled watchdog task: $watchdogTaskName" -ForegroundColor Green
        }
        foreach ($processName in $ProcessNames) {
            $proc = Get-Process -Name $processName -ErrorAction SilentlyContinue
            if ($proc) {
                foreach ($item in $proc) {
                    $processPath = $null
                    try {
                        $processPath = $item.Path
                    } catch {
                        $processPath = $null
                    }
                    if ($processPath -and (Test-PathUnderDirectory -Path $processPath -Directory $ReleaseRoot)) {
                        $restartPackagedControllerAfterBuild = $true
                    }
                }
                $proc | Stop-Process -Force
                Write-Host "Stopped: $processName" -ForegroundColor Green
            } else {
                Write-Host "No running process found for $processName."
            }
        }
    }

    Write-Host "`n[1/11] Reading repo version..." -ForegroundColor Yellow
    $version = Get-ProjectVersion -VersionFilePath $VersionFile
    Write-Host "Version: $version" -ForegroundColor Green

    Write-Host "`n[2/11] Cleaning build directories..." -ForegroundColor Yellow
    # -Strict: a clean that cannot remove a locked tree fails the build here
    # instead of warning and then building against stale artifacts.
    Remove-DirectoryIfExists -Path $BuildRoot -Strict
    New-EmptyDirectory -Path $BuildDir -Strict
    New-EmptyDirectory -Path $DistDir -Strict

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
    $ctestExe = Resolve-CTestPath -CMakeExe $cmakeExe
    if (-not $ctestExe) {
        throw 'ctest not found next to cmake or in PATH.'
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
    $workingTreeDirty = Get-GitWorkingTreeDirty -RepositoryRoot $RepoRoot

    Write-Host "cl.exe : $(Get-Command cl.exe | Select-Object -ExpandProperty Source)" -ForegroundColor Green
    Write-Host "cmake  : $cmakeExe" -ForegroundColor Green
    Write-Host "ctest  : $ctestExe" -ForegroundColor Green
    Write-Host "ninja  : $ninjaExe" -ForegroundColor Green
    Write-Host "vcpkg  : $vcpkgRoot" -ForegroundColor Green
    Write-Host "triplet: $triplet" -ForegroundColor Green
    if ($sourceCommit) {
        if ($workingTreeDirty) {
            Write-Host "commit : $sourceCommit (working tree DIRTY)" -ForegroundColor Yellow
        } else {
            Write-Host "commit : $sourceCommit" -ForegroundColor Green
        }
    } else {
        Write-Host "commit : unavailable" -ForegroundColor DarkGray
    }
    if ($workingTreeDirty) {
        Write-Warning "Working tree has uncommitted changes. This package is stamped sourceCommit=$sourceCommit but is NOT reproducible from that commit. build-info.json records workingTreeDirty=true and the embedded --version is suffixed -dirty. Commit before a release or live-evidence build for clean provenance."
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
    Push-Location -LiteralPath $RepoRoot
    try {
        Invoke-External -FilePath $cmakeExe -Arguments @(
            '--build', '--preset', $PresetName, '--parallel'
        ) -FailureMessage 'CMake build failed'
    } finally {
        Pop-Location
    }

    Write-Host "`n[6/11] Packaging to dist/..." -ForegroundColor Yellow
    $builtMainExe = Join-Path $BuildDir $MainExeName
    if (-not (Test-Path -LiteralPath $builtMainExe)) {
        $builtMainExe = Join-Path $BuildDir "Release\$MainExeName"
    }
    if (-not (Test-Path -LiteralPath $builtMainExe)) {
        throw "Build completed but $MainExeName was not found under $BuildDir."
    }

    # $DistDir was created empty in step [2/11] and is not removed before here.
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
    Copy-DistFileAs `
        -SourcePath (Join-Path $RepoRoot 'config\runtime_policy_write_live.json') `
        -DestinationPath (Join-Path $DistDir 'runtime_policy_write_live.json')

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
        Write-Host "`n[8/11] Tests skipped." -ForegroundColor DarkGray
    } else {
        Write-Host "`n[8/11] Running CTest..." -ForegroundColor Yellow
        Invoke-CMakeTests -CTestExe $ctestExe -BuildDirectory $BuildDir
        Write-Host "CTest lane passed." -ForegroundColor Green

        Write-Host "`n[8b/11] Running hermetic tests..." -ForegroundColor Yellow
        Invoke-HermeticTests -RepositoryRoot $RepoRoot -ControlExePath $builtMainExe
        $testsPassed = $true
        Write-Host "Hermetic test lane passed." -ForegroundColor Green
    }

    if ($NoPublish) {
        Write-Host "`n[9/11] Publishing skipped (-NoPublish)." -ForegroundColor DarkGray
        Write-Host "`n[10/11] Release archive skipped (-NoPublish)." -ForegroundColor DarkGray
    } else {
        Write-Host "`n[9/11] Publishing to release/..." -ForegroundColor Yellow
        Publish-DistToRelease -DistDir $DistDir -ReleaseRoot $ReleaseRoot -PreserveNames @('archive', 'runtime')
        Write-Host "Published dist/ to release/ (atomic per-file replace)." -ForegroundColor Green
        Write-Host "Preserved release/archive and release/runtime." -ForegroundColor DarkGray

        $buildInfoPath = New-BuildInfo `
            -ArtifactRoot $ReleaseRoot `
            -MainArtifactPath (Join-Path $ReleaseRoot $MainExeName) `
            -ProjectName $ProjectName `
            -Version $version `
            -Architecture $Architecture `
            -PresetName $PresetName `
            -TestsRun $testsRun `
            -TestsPassed $testsPassed `
            -SourceCommit $sourceCommit `
            -WorkingTreeDirty $workingTreeDirty
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
    }

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

    if ($restartPackagedControllerAfterBuild) {
        Start-PackagedController -ReleaseRoot $ReleaseRoot -MainExeName $MainExeName | Out-Null
    }
    Resume-ScheduledTaskIfNeeded -TaskName $watchdogTaskName -ShouldEnable $resumeWatchdogAfterBuild
    if ($resumeWatchdogAfterBuild) {
        Clear-WatchdogSuspendSentinel -RepoRoot $RepoRoot
    }
}

$timer.Stop()
$elapsed = $timer.Elapsed

Write-Host "`n--- SUCCESS: $ProjectName v$version ---" -ForegroundColor Green
Write-Host "Release dir : $ReleaseRoot"
if ($sourceCommit) {
    if ($workingTreeDirty) {
        Write-Host "Commit      : $sourceCommit (working tree DIRTY — not reproducible from this commit)" -ForegroundColor Yellow
    } else {
        Write-Host "Commit      : $sourceCommit"
    }
}
if (Test-Path -LiteralPath $ReleaseRoot) {
    Get-ChildItem -LiteralPath $ReleaseRoot -File | Format-Table Name, @{Label='Size'; Expression={'{0:N0} bytes' -f $_.Length}} -AutoSize
}
Write-Host "SHA256      : $mainExeHash"
if ($testsRun) {
    Write-Host "Tests       : passed"
} else {
    Write-Host "Tests       : skipped"
}
if ($zipPath) {
    Write-Host ("Archive     : {0} ({1:N0} bytes)" -f (Split-Path -Path $zipPath -Leaf), $zipSize)
} else {
    Write-Host "Archive     : skipped"
}
Write-Host ("Build completed in {0:mm\:ss\.fff}" -f $elapsed)
