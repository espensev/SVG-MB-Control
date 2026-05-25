# PawnIO Modules (vendored binaries)

This directory documents the provenance of the PawnIO module binaries shipped
under `resources/pawnio/`. The binaries themselves are kept in `resources/`
rather than here because they are runtime assets packaged into the release.

## Upstream

- Project: PawnIO Modules
- Source: <https://github.com/namazso/PawnIO.Modules>
- License: LGPL-2.1-or-later (see `LICENSE` in this directory)
- Copyright: 2025 namazso <admin@namazso.eu>

## Release Tracked

- Release tag: `0.2.6`
- Release page:
  <https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.6>
- Asset: `release_0_2_6.zip`
  - Download URL:
    <https://github.com/namazso/PawnIO.Modules/releases/download/0.2.6/release_0_2_6.zip>
  - Size: 53,833 bytes
  - SHA-256:
    `25f9bdff24cc0326591eebc0b2b73551119a1a7de8e778a29b30b93f95b948e1`
  - GitHub publishes this digest in the release API response as the asset's
    `digest` field, so the chain of custody is verifiable without trusting a
    mirror.

## Binaries Used By This Repo

The release zip ships 19 module binaries. svg-mb-control packages only the
two it actually loads.

| Module bin | Size | SHA-256 | Shipped to |
|---|---:|---|---|
| `AMDFamily17.bin` | 9,300 bytes | `099dc01d6db97ea997fec4a461e191cc64b9d7ce47c9d2153c451c56c2adcf50` | `resources/pawnio/AMDFamily17.bin` |
| `LpcIO.bin` | 17,172 bytes | (not verified in this PR) | `resources/pawnio/LpcIO.bin` |

The `AMDFamily17.bin` SHA-256 above matches what is currently committed at
`resources/pawnio/AMDFamily17.bin` byte for byte, and is also the value
registered in `kPawnIoSpecAmdFamily17V1` in `src/hardware/pawnio_binary.cpp`.

## IOCTL Functions Exposed By `AMDFamily17.bin`

From upstream `AMDFamily17.p`:

- `ioctl_read_msr` (1 input, 1 output)
- `ioctl_write_msr` (2 inputs, 0 outputs)
- `ioctl_read_smn` (1 input, 1 output)

`svg-mb-control` currently invokes only `ioctl_read_smn`; the MSR-reading
entry points are documented here so future readers know what additional
surface is already available from the bin we ship.

## Refreshing The Vendored Bins

To validate or upgrade against a new upstream release:

1. Fetch the release zip and its expected SHA-256 from the GitHub release API:

   ```pwsh
   $api = gh api repos/namazso/PawnIO.Modules/releases/latest |
       ConvertFrom-Json
   $asset = $api.assets[0]
   $asset.name
   $asset.browser_download_url
   $asset.digest    # sha256:...
   ```

2. Download and verify the zip hash matches `$asset.digest`.
3. Extract `AMDFamily17.bin`. Compute its SHA-256.
4. Update `resources/pawnio/AMDFamily17.bin`, the hash in
   `kPawnIoSpecAmdFamily17V1` (`src/hardware/pawnio_binary.cpp`), and the
   tables in this README.
5. Run `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` and inspect any warn-only
   hash-mismatch messages surfaced via `AmdReader::init_warning()` during a
   manual smoke run.
