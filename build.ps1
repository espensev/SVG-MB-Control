<#
.SYNOPSIS
	Configure and build SVG-MB-Control under a guaranteed x64 MSVC toolchain.

.DESCRIPTION
	Wraps vcvarsall.bat (x64) so the build never inherits a mismatched x86
	Developer Prompt environment, which is what produces LNK4272
	"library machine type 'x86' conflicts with target machine type 'x64'"
	plus the cascade of unresolved __imp_* / CRT symbols.

	Steps:
	  1. Locate Visual Studio via vswhere (falls back to a known path).
	  2. Import the x64 environment from vcvarsall.bat into this session.
	  3. Run `cmake --preset` and `cmake --build` for the chosen preset.

.PARAMETER Preset
	CMake preset name. Defaults to 'x64-release'.

.PARAMETER Target
	Optional ninja/cmake target. Defaults to all targets.

.PARAMETER Clean
	If set, deletes build\<Preset> before configuring.

.PARAMETER Reconfigure
	If set, forces `cmake --preset` even when the build dir already exists.

.EXAMPLE
	.\build.ps1
	.\build.ps1 -Preset x64-debug
	.\build.ps1 -Clean -Target svg_mb_control
#>
[CmdletBinding()]
param(
	[string]$Preset = 'x64-release',
	[string]$Target,
	[switch]$Clean,
	[switch]$Reconfigure
)

$ErrorActionPreference = 'Stop'
$RepoRoot = $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build\$Preset"

function Find-VcVarsAll {
	$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
	if (Test-Path $vswhere) {
		$vsRoot = & $vswhere -latest -prerelease -products * `
			-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
			-property installationPath 2>$null | Select-Object -First 1
		if ($vsRoot) {
			$candidate = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvarsall.bat'
			if (Test-Path $candidate) { return $candidate }
		}
	}

	# Fallback: known VS 18 Enterprise install on this machine.
	$fallback = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvarsall.bat'
	if (Test-Path $fallback) { return $fallback }

	throw "vcvarsall.bat not found. Install the 'MSVC v143+ - VS 20xx C++ x64/x86 build tools' workload."
}

function Import-VcEnv {
	param([Parameter(Mandatory)][string]$VcVarsAll, [string]$Arch = 'x64')

	Write-Host "==> Importing MSVC $Arch environment from:" -ForegroundColor Cyan
	Write-Host "    $VcVarsAll"

	# Run vcvarsall in a child cmd, then dump env vars, then import them here.
	$envDump = & cmd /c "`"$VcVarsAll`" $Arch >nul && set"
	if ($LASTEXITCODE -ne 0) {
		throw "vcvarsall.bat $Arch failed with exit code $LASTEXITCODE."
	}

	foreach ($line in $envDump) {
		if ($line -match '^([^=]+)=(.*)$') {
			Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2]
		}
	}

	# Sanity check: link.exe must now be the x64 host/target build.
	$link = (Get-Command link.exe -ErrorAction SilentlyContinue).Source
	if (-not $link -or $link -notmatch '\\Hostx64\\x64\\') {
		throw "link.exe is not the x64 host/target build: '$link'. Aborting to prevent LNK4272."
	}

	# Strip any leftover x86 LIB / PATH entries inherited from the launching
	# shell. vcvarsall x64 only *prepends* x64 paths; if the parent shell was
	# an x86 Developer Prompt, the x86 entries linger and can be picked up if
	# an x64 candidate is ever missing -> LNK4272 cascades.
	if ($env:LIB) {
		$cleanLib = ($env:LIB -split ';' |
			Where-Object { $_ -and ($_ -notmatch '\\lib(\\[^\\]+)?\\x86($|\\)') }) -join ';'
		$env:LIB = $cleanLib
	}
	if ($env:LIBPATH) {
		$cleanLibPath = ($env:LIBPATH -split ';' |
			Where-Object { $_ -and ($_ -notmatch '\\lib(\\[^\\]+)?\\x86($|\\)') }) -join ';'
		$env:LIBPATH = $cleanLibPath
	}

	Write-Host "    link.exe: $link" -ForegroundColor DarkGray
}

function Invoke-Native {
	param([Parameter(Mandatory)][string]$Exe, [string[]]$Args)
	Write-Host ">> $Exe $($Args -join ' ')" -ForegroundColor Yellow
	& $Exe @Args
	if ($LASTEXITCODE -ne 0) {
		throw "$Exe exited with code $LASTEXITCODE"
	}
}

Push-Location $RepoRoot
try {
	if ($Clean -and (Test-Path $BuildDir)) {
		Write-Host "==> Cleaning $BuildDir" -ForegroundColor Cyan
		Remove-Item -Recurse -Force $BuildDir
	}

	$vcvars = Find-VcVarsAll
	Import-VcEnv -VcVarsAll $vcvars -Arch 'x64'

	$needConfigure = $Reconfigure -or -not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))
	if ($needConfigure) {
		Invoke-Native -Exe 'cmake' -Args @('--preset', $Preset)
	} else {
		Write-Host "==> Skipping configure (cache exists). Use -Reconfigure to force." -ForegroundColor DarkGray
	}

	$buildArgs = @('--build', $BuildDir)
	if ($Target) { $buildArgs += @('--target', $Target) }
	Invoke-Native -Exe 'cmake' -Args $buildArgs

	Write-Host "==> Build succeeded: $Preset" -ForegroundColor Green
}
finally {
	Pop-Location
}
