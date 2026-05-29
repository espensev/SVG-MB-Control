#requires -Version 5.1

# CMake/CTest and hermetic Python test lanes.
# Extracted from Build-Release.ps1; dot-sourced by it. Functions are
# parameter-driven and invoked from the Build-Release.ps1 pipeline scope.

function Invoke-CMakeTests {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$CTestExe,
        [Parameter(Mandatory = $true)][string]$BuildDirectory
    )

    Invoke-External -FilePath $CTestExe -Arguments @(
        '--test-dir', $BuildDirectory,
        '--output-on-failure'
    ) -FailureMessage 'CTest lane failed'
}

function Invoke-HermeticTests {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$ControlExePath
    )

    $pythonRunner = Resolve-PythonRunner
    if (-not $pythonRunner) {
        throw 'Python 3 was not found. Install python or rerun with -SkipTests.'
    }

    $arguments = @($pythonRunner['PrefixArgs']) + @('-m', 'unittest', 'discover', 'tests', '-v')
    $previousTestExe = $env:SVG_MB_CONTROL_TEST_EXE
    $env:SVG_MB_CONTROL_TEST_EXE = $ControlExePath
    Push-Location -LiteralPath $RepositoryRoot
    try {
        Invoke-External -FilePath $pythonRunner['FilePath'] -Arguments $arguments -FailureMessage 'Hermetic test lane failed'
    } finally {
        Pop-Location
        if ($null -eq $previousTestExe) {
            Remove-Item Env:\SVG_MB_CONTROL_TEST_EXE -ErrorAction SilentlyContinue
        } else {
            $env:SVG_MB_CONTROL_TEST_EXE = $previousTestExe
        }
    }
}
