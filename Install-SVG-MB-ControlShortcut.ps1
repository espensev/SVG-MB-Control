#requires -Version 5.1

[CmdletBinding()]
param(
    [string]$ShortcutName = 'SVG-MB Control',
    [switch]$AllUsers,
    [switch]$NoRunAsAdmin,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-ControlExe {
    $scriptRoot = Split-Path -Parent $PSCommandPath
    $candidates = @(
        (Join-Path $scriptRoot 'svg-mb-control.exe'),
        (Join-Path $scriptRoot 'release\svg-mb-control.exe')
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).ProviderPath
        }
    }

    throw 'Could not find svg-mb-control.exe next to this script or under release\.'
}

function Set-ShortcutRunAsAdmin {
    param([Parameter(Mandatory = $true)][string]$Path)

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -le 0x15) {
        throw "Shortcut file is unexpectedly small: $Path"
    }

    $bytes[0x15] = $bytes[0x15] -bor 0x20
    [System.IO.File]::WriteAllBytes($Path, $bytes)
}

function Remove-LegacyShortcut {
    param(
        [Parameter(Mandatory = $true)][object]$Shell,
        [Parameter(Mandatory = $true)][string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return
    }

    $shortcut = $Shell.CreateShortcut($Path)
    $targetName = Split-Path -Leaf $shortcut.TargetPath
    if ($targetName -ieq 'svg-mb-control.exe') {
        Remove-Item -LiteralPath $Path -Force
        Write-Host "Removed stale shortcut: $Path"
    }
}

$programsRoot = if ($AllUsers) {
    [Environment]::GetFolderPath('CommonPrograms')
} else {
    [Environment]::GetFolderPath('Programs')
}

if ([string]::IsNullOrWhiteSpace($programsRoot)) {
    throw 'Could not resolve the Windows Start Menu programs folder.'
}

$shortcutDir = Join-Path $programsRoot 'SVG-MB Control'
$shortcutPath = Join-Path $shortcutDir "$ShortcutName.lnk"
$legacyShortcutPath = Join-Path $programsRoot 'svg-mb-control.exe.lnk'
$shell = New-Object -ComObject WScript.Shell

if ($Remove) {
    if (Test-Path -LiteralPath $shortcutPath) {
        Remove-Item -LiteralPath $shortcutPath -Force
        Write-Host "Removed shortcut: $shortcutPath"
    } else {
        Write-Host "Shortcut already absent: $shortcutPath"
    }
    Remove-LegacyShortcut -Shell $shell -Path $legacyShortcutPath
    return
}

$exePath = Resolve-ControlExe
$exeDir = Split-Path -Parent $exePath

New-Item -ItemType Directory -Path $shortcutDir -Force | Out-Null

$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $exePath
$shortcut.Arguments = ''
$shortcut.WorkingDirectory = $exeDir
$shortcut.IconLocation = "$exePath,0"
$shortcut.WindowStyle = 7
$shortcut.Description = 'Start SVG-MB Control fan controller'
$shortcut.Save()

if (-not $NoRunAsAdmin) {
    Set-ShortcutRunAsAdmin -Path $shortcutPath
}

Remove-LegacyShortcut -Shell $shell -Path $legacyShortcutPath

Write-Host "Created shortcut: $shortcutPath"
Write-Host 'Open Start, find "SVG-MB Control", then right-click it to pin it to Start.'
