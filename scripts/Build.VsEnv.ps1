#requires -Version 5.1

# Visual Studio environment bootstrap (vswhere / VsDevCmd / VsDevShell).
# Extracted from Build-Release.ps1; dot-sourced by it. Functions are
# parameter-driven and invoked from the Build-Release.ps1 pipeline scope.

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

    # Last-resort fallback for machines where vswhere is absent. Maintain the
    # version list as new Visual Studio releases ship; the vswhere path above is
    # preferred and version-agnostic.
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
