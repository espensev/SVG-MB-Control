# FEAT-0020 Build-Chain Review

**Project:** svg-mb-control
**Status:** Review note; build scripts unchanged
**Updated:** 2026-06-18
**Owner spec:** `docs/features/FEAT-0020-standard-control-loop-power-logging.md`

## Verdict

The existing build chain is the right path for FEAT-0020 repo-local
implementation. No new build entrypoint is needed. Use:

```powershell
.\scripts\Test-LocalCI.ps1 -KeepBuildDir
```

This runs `scripts\Build-Release.ps1 -NoStopProcesses -NoPublish`, so it builds,
packages to `dist\`, runs CTest, runs Python unittest discovery, and does not
publish into `release\` or stop a live controller.

Do not use raw CMake as the normal validation path. Do not use `.\build-release.ps1`
without `-NoPublish` unless release/live work is separately authorized.

## Current chain coverage

- CMake uses an explicit source list. Existing FEAT-0020 edit areas
  (`gpu_reader.cpp`, `direct_runtime_snapshot.cpp`, `runtime_csv_rows.cpp`,
  analyzer `.cpp` files) are already in `svg_mb_control_core`.
- C++ CSV-row coverage is already registered through
  `svg_mb_control_csv_rows_tests` and CTest.
- Python `unittest discover tests -v` includes `tests/test_feature_specs.py`, so
  feature registry/traceability drift is checked by the hermetic lane.
- `Build.Tests.ps1` sets `SVG_MB_CONTROL_TEST_EXE` to the freshly built exe for
  Python smoke/integration tests.
- `Test-LocalCI.ps1` serializes concurrent local-CI runs with a per-repo lock,
  reducing build-tree and temp-artifact collisions.

## Required FEAT-0020 build-chain obligations

These are not needed until the corresponding implementation files exist:

1. If FEAT-0020 adds a new C++ source file instead of editing existing analyzer
   or runtime files, add it to the explicit `svg_mb_control_core` source list in
   `CMakeLists.txt`.
2. If FEAT-0020 adds a new C++ test executable, register it with
   `svg_mb_control_add_core_test(...)` or an appropriate standalone test block.
   If the test extends `tests/cpp/csv_rows_tests.cpp` or
   `tests/cpp/analyze_report_tests.cpp`, no CMake change is needed.
3. When `scripts\Set-EnergyLoggingProfile.ps1` is added, add it to
   `$DistExtras` in `scripts\Build-Release.ps1` so release packages contain the
   operator workflow.
4. Update README release-output documentation when the packaged operator script
   is added.
5. Keep local validation on `Test-LocalCI`; release publishing and live restart
   remain separate authorization gates.

## Current gap

The current build chain will not package a future
`scripts\Set-EnergyLoggingProfile.ps1` automatically. `Build-Release.ps1` copies
only selected script files from `$DistExtras`; it does not package the entire
`scripts\` directory. This is intentional for package shape, but FEAT-0020 must
update `$DistExtras` in the same change that creates the operator script.

Do not add the missing script to `$DistExtras` before the script exists: the
package helper currently warns and skips missing extras, which would add noisy
build output without shipping anything.

## Validation sequence

During FEAT-0020 implementation:

```powershell
python -m unittest tests.test_feature_specs -v
.\scripts\Test-LocalCI.ps1 -KeepBuildDir
```

The first command is a quick governance check. The second is the required
repo-local validation for C++ behavior changes.
