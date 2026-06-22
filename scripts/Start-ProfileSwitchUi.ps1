param(
    [int]$Port = 8766,
    [string]$HostAddress = "127.0.0.1",
    [switch]$NoOpen
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$manifest = Join-Path $repoRoot "tools\profile_switch_ui\Cargo.toml"

$uiArgs = @(
    "--repo", $repoRoot,
    "--host", $HostAddress,
    "--port", "$Port"
)

if (-not $NoOpen) {
    $uiArgs += "--open"
}

cargo run --manifest-path $manifest -- @uiArgs
