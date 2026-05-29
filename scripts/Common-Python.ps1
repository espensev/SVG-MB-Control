#requires -Version 5.1

# Shared Python interpreter resolver for repo PowerShell scripts. Returns a
# hashtable @{ FilePath; PrefixArgs } where PrefixArgs is @('-3') for the Windows
# launcher (py) and @() for python, or $null when neither is found. Dot-sourced
# by scripts/Build-Release.ps1 and scripts/Start-EvalDashboard.ps1 (and shipped
# beside the packaged dashboard launcher so the released dashboard can run).
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
