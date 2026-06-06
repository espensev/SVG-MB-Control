# Testing harness / flow — evaluation & handoff (2026-06-06)

Scope: the local CI test harness (`scripts\Test-LocalCI.ps1` and the
`Build-Release.ps1` pipeline it wraps), both test lanes, isolation/leftover-state
behavior, and coverage.

How verified: read of the harness scripts, `CMakeLists.txt`, all 8 `tests/cpp/`
files and all 14 `tests/*.py` files; one full `Test-LocalCI.ps1 -KeepBuildDir`
run (commit `33f8d24`, main exe SHA256 `78D347921611138BBD06C35BF2D941FA6CE0C977B1E18EB404A54FF411B6D813`);
a 12× isolated re-run of the one failing test exe; and a multi-agent static
cross-check (coverage maps, doc audit, adversarially-verified gaps). Claims here
are grounded in that evidence; items labeled "Recommendation" are advisory.

Status of this doc: findings snapshot, not a maintained contract. `git log`,
`docs\STRUCTURE_AND_STABILITY.md`, `README.md`, and the tests stay authoritative.

---

## TL;DR

The harness is sound and genuinely green: a clean run passes CTest 8/8 and the
hermetic `unittest` lane 114/114 (0 skips). Running it does **not** disturb the
live fan-control loop (verified empirically and by static kill-scoping analysis).
The real weaknesses:

1. It shares mutable state across invocations (single `build\` tree; fixed-name
   `%TEMP%` files in the C++ lane), so two concurrent runs collide.
2. Several paths **skip rather than fail**, so a green result can certify nothing.
3. The **real hardware decode/translate math is structurally unreachable** from
   the harness, leaving 5 verified coverage gaps (one High).

---

## 1. The flow

`scripts\Test-LocalCI.ps1` is a thin wrapper (`Test-LocalCI.ps1:16-20`) that calls
`Build-Release.ps1 -NoStopProcesses -NoPublish` (plus `-KeepBuildDir` when its own
switch is set) and propagates `$LASTEXITCODE`. The 11-step pipeline runs tests at:

- `[8/11]` CTest lane — `ctest --test-dir <build> --output-on-failure`
  (`Build.Tests.ps1` `Invoke-CMakeTests`). 8 C++ executables registered under
  `if(BUILD_TESTING)` (`CMakeLists.txt:243-323`), each a single `tests\cpp\*.cpp`
  linked to `svg_mb_control_core`. Bespoke assertion style: file-local
  `g_failures` + `Expect*` helpers, `main()` returns 1 if any failure. No
  GoogleTest/Catch2; no auto-registration.
- `[8b/11]` hermetic lane — `python -m unittest discover tests -v`
  (`Build.Tests.ps1` `Invoke-HermeticTests`), with `SVG_MB_CONTROL_TEST_EXE` set
  to the freshly built exe. 14 `test_*.py` modules drive the real exe via
  subprocess using `SVG_MB_CONTROL_SIM_*` env so no real hardware is touched.

Observed run (commit `33f8d24`): build ~19s, CTest 8/8 in 3.91s, hermetic
114 tests in 66.287s (0 failures, 0 skips), total 01:29.544. `[0/11]` reported
"Skipping process stop (-NoStopProcesses)"; `[9-10/11]` skipped publish/archive.

---

## 2. Live-runtime safety — running the harness does not disturb the live loop

Verdict: confirmed safe, two independent ways.

- Empirical: during the run both live `svg-mb-control` PIDs (2200, 12444) kept
  running; `release\` was untouched (`-NoPublish`).
- Static: every process-kill in `tests\` is PID-scoped. The only name-based kill
  is `helpers.py` `_terminate_svg_processes`:
  `Get-Process svg-mb-control | Where-Object { $ids -contains $_.Id } | Stop-Process`,
  where `$ids` come **only** from staged-sidecar PIDs read out of the per-test
  `runtime_home` (`_published_process_ids`: `control_runtime.json` `process_id`,
  `control_supervisor.json` `supervisor_pid`/`last_worker_pid`), each guarded
  `pid>0`. All other kills target the test's own `Popen` handle or
  `Stop-Process -Id {staged-pid}`. The cross-process singleton mutex is
  `sha256(canonical runtime_home)` (`src\platform\runtime_singleton.cpp:92-108`),
  so a staged temp `runtime_home` cannot collide with the live `release\runtime`
  lock. `Build-Release.ps1` contains a name-only blanket kill
  (`Build-Release.ps1:153-167`) but it is gated by `if ($NoStopProcesses)`
  (`:145`), and both the runner and `helpers._ensure_release_build` pass
  `-NoStopProcesses`.

Residual nit (Recommendation): the name filter does enumerate a live instance
(same image name); safety rests entirely on the PID `Where-Object` (one line of
defense, plus a theoretical pid-recycle window). A `.Path`-under-staged-root
check before `Stop-Process` (as `Build-Release.ps1` already does for its restart
logic via `Test-PathUnderDirectory`) would add defense-in-depth.

---

## 3. Isolation / leftover-state

- Shared `build\` tree, not per-run (highest stale-state risk). When `[2/11]`
  clean cannot remove a locked file it emits a WARNING and proceeds; a subsequent
  `ninja: no work to do` then runs the lanes against whatever artifacts already
  exist in the tree. Observed: a concurrent second `Test-LocalCI` run held
  `sqlite3.c.obj`, so the clean degraded and the lanes ran against the existing
  tree.
- C++ lane writes fixed-name artifacts to shared `%TEMP%`:
  `svg_mb_control_config_test_<N>.json` (TempJsonFile, per-process counter from 0
  → always `_1.json` first) and `svg_mb_control_channel_write_tests_<name>`
  (`channel_write_tests.cpp:83-91`). Both clean up on normal exit (RAII /
  `remove_all` at start+end; verified 0 residue), so residue only leaks on
  crash/kill. The structural problem is that the names are not unique per process,
  so two concurrent runs collide: one truncates `_1.json` inside the other's read
  window, failing `svg_mb_control_loop_config_tests` with
  "parse error ... attempting to parse an empty input". This is flaky, NOT a
  regression — the exe is 12/12 green in isolation. The TempJsonFile comment
  claiming names are "distinct so concurrent runs get distinct files" is incorrect
  across processes.
- Python lane is the clean model: every controller run is confined to a
  `tempfile.TemporaryDirectory` with `runtime_home_path` embedded in a per-test
  config and context-manager cleanup. No AppData/ProgramData writes; never touches
  `release\runtime`. `_merged_env` defaults `SVG_MB_CONTROL_SIM_DIRECT_AMD_MODE=disabled`
  so a test that forgets sim env still does not read real hardware.
- Repo-root scratch carryover (working tree, not `%TEMP%`): `__audit_tmp.txt`
  (dated 2026-05-29) and `__localci_run.log` sit untracked in the repo root.
  `build\` and `release\` are gitignored; the `__*` scratch pattern is not.

---

## 4. Skip-rather-than-fail (a green run can certify nothing)

- Non-Windows runner: every exe-backed `test_*.py` has
  `setUpClass: if sys.platform != "win32": raise unittest.SkipTest`. On a
  non-Windows CI host the entire behavioral surface skips and `unittest` exits 0
  having exercised zero controller behavior. Latent portability hazard (passes on
  this Windows host).
- `tests\helpers.py:29-61` `_ensure_release_build`: on the bare
  `python -m unittest` path with no `SVG_MB_CONTROL_TEST_EXE` and no built exe, a
  failed `build-release.ps1` is downgraded to `unittest.SkipTest`, not a failure.
  Not reachable via `Test-LocalCI` (which builds first and sets the env var), but
  it is the most exploitable green-validates-nothing path for a developer running
  `unittest` directly.
- CTest lacks `--no-tests=error` (`Build.Tests.ps1:14-17`); a configuration that
  forced `-DBUILD_TESTING=OFF` would pass CTest green with 0 tests. Latent (the
  default `BUILD_TESTING=ON` keeps it at 8; the pipeline never sets the flag).
- `-SkipTests` (`Build-Release.ps1:378-389`) skips BOTH lanes and can still
  publish (without `-NoPublish`) a release stamped `testsRun=false` and print
  SUCCESS. Operator footgun on direct `Build-Release.ps1`; `Test-LocalCI` never
  sets it.
- Guarantee boundary: a green `Test-LocalCI` certifies only the freshly built
  `build\x64-release` artifacts. There is no hash comparison between the built exe
  and the running/`release\` exe, so it does not certify what is deployed.

---

## 5. Coverage gaps (adversarially verified as untested)

The hermetic lane is broad (config contracts, control-loop behavior including
blends/boosts/cadence/breaker, write-once reconcile, supervisor crash/restart,
analyze ingest/report, runtime health, singleton). The verified holes cluster in
the real hardware path:

| Module | Untested behavior | Severity |
|---|---|---|
| `src\hardware\amd_reader.cpp` `DecodeTctl` / `DecodeCcdTemp` | raw-register→°C decode math (shift/scale/offset; CCD validity gate). Pure, trivially unit-testable, zero coverage. | High |
| `src\hardware\pawnio_binary.cpp` `LoadPawnIoBinary` | SHA-256 verification gating (strict / warn_only / skip; env skip override; mismatch handling) on a ring-0 kernel module. | Medium |
| `src\hardware\sio_fan_writer.cpp` `TranslateStatus` / `IsTransientSioStatus` / `RetryTransientSioOperation` | status→`FanWriteError` mapping that feeds breaker/restore decisions (`channel_write.cpp` branches on `kPolicyRefused`/`kTimedOut`); 3-attempt retry. | Medium |
| `src\hardware\amd_reader.cpp` `SelectCcdLayout` + CPUID family/model | Zen model→per-CCD register-base mapping and unsupported fallback. | Medium |
| `src\analyze\analyze_csv.cpp` `ParseTickRow` | silent row-drop on empty `wall_clock` / missing `loop_tick_count` (offline analysis integrity only). | Low |

Root cause for the AMD items: the functions are in anonymous namespaces (internal
linkage) and reachable only via `Impl::OpenReal`, which the `AmdReader` ctor skips
on both `SVG_MB_CONTROL_SIM_DIRECT_AMD_MODE=enabled` and `disabled`
(`amd_reader.cpp:523-535`). Sim injects a final Celsius value; there is no
`SIM_AMD_*_RAW` hook, unlike the SIO/fan readers which expose `SIM_SIO_TEMPERATURE0_RAW`
/ `SIM_FAN_DUTY_RAW` that DO drive their decoders. So the AMD decode math is
structurally unreachable, not merely omitted.

Two C++-lane structural notes:
- `boost_stage_tests.cpp` is a refactor-equivalence guard against a frozen copy
  (`LegacyPressureBoost`/`LegacyCpuLowSoakBoost`) pasted into the test, not an
  independent oracle: a bug shared by source and copy passes, and the copy can
  drift from live source.
- The bespoke harness has no auto-registration: a `TestX()` omitted from `main()`
  silently never runs and the suite still reports green.

---

## 6. Documentation drift

- `README.md` Analyze section states DB schema version `7`; code is `8`
  (`src\analyze\analyze_db.h` `kSchemaVersion = 8`; `analyze_db.cpp` migrates
  `<=7` and adds `low_band_stage_boost_pct` / `low_band_effective_boost_pct`).
  README also omits those two columns.
- The C++ CTest lane is documented nowhere (README / AGENTS.md /
  RUNTIME_LOGGING_AND_EVALUATION.md). A contributor changing core C++ would not
  know 8 native targets gate the build.
- `README.md` Tests section presents `python -m unittest discover tests -v` as a
  standalone command but does not note that on a clean checkout it auto-triggers a
  build (`_ensure_release_build`) and that a missing build yields skips, not
  failures.
- `README.md` build options describe `-SkipTests` as skipping only the Python
  run; it skips both lanes (the script `-Help` text is correct).
- `docs\RUNTIME_LOGGING_AND_EVALUATION.md` "Current as of 2026-05-29" predates the
  FEAT-0002 `system_cpu_busy_pct` content it already documents.
- The build/test memory note calling `[8b]` a "pytest lane" is wrong; it is
  `unittest`. (Corrected in the auto-memory on 2026-06-06.)

---

## 7. Recommendations (prioritized)

1. Single-instance guard on `Test-LocalCI` (lockfile/mutex) so a second concurrent
   invocation refuses or queues. Removes the shared-`build\` and `%TEMP%`
   collision failures at the source. (Highest ROI.)
2. Unique C++ test temp names (fold in `GetCurrentProcessId()` or
   `std::filesystem::temp_directory_path() / unique_path()`); fix the misleading
   TempJsonFile comment. Self-contained; re-validates through this same harness.
   APPLIED 2026-06-06: `tests\cpp\control_loop_config_tests.cpp` (TempJsonFile)
   and `tests\cpp\channel_write_tests.cpp` (MakeTempHome) now build a
   per-process `random_device` salt + counter (`UniqueTempSuffix`); comments
   corrected. Re-validated via `Test-LocalCI.ps1 -KeepBuildDir`: CTest 8/8,
   hermetic 114/114, exit 0.
3. Fail loud when `[2/11]` clean cannot remove the tree instead of warning then
   running against stale artifacts; add CTest `--no-tests=error`.
4. Close the High gap: extract `DecodeTctl`/`DecodeCcdTemp` to a header and add a
   C++ decode test, or add a `SIM_AMD_*_RAW` hook (mirror the SIO/fan sim path).
5. Doc fixes: schema 7→8 (+ the two columns), document the CTest lane, correct
   `-SkipTests`, advance the RUNTIME_LOGGING date stamp.
6. Repo hygiene: remove `__audit_tmp.txt`; gitignore `__*` / `*_run.log`.
7. Optional: add a `.Path`-under-staged-root guard to the test kill helper
   (defense-in-depth on top of PID scoping).

---

## 8. Reproduce / evidence index

- Run the harness: `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` (does not stop the
  live loop or publish). Expected: CTest 8/8, hermetic 114/114, exit 0.
- Reproduce the flaky failure: start two `Test-LocalCI` runs at once; one may fail
  `svg_mb_control_loop_config_tests` with "empty input". The exe alone is 12/12
  green: `build\x64-release\svg_mb_control_loop_config_tests.exe`.
- Key files: `scripts\Test-LocalCI.ps1`, `scripts\Build-Release.ps1`,
  `scripts\Build.Tests.ps1`, `scripts\Common-Python.ps1`, `CMakeLists.txt`
  (`:243-323`), `tests\helpers.py`, `tests\cpp\*.cpp`, `tests\*.py`,
  `src\hardware\amd_reader.cpp`, `src\hardware\pawnio_binary.cpp`,
  `src\hardware\sio_fan_writer.cpp`, `src\analyze\analyze_csv.cpp`,
  `src\platform\runtime_singleton.cpp`.

## 9. Not done in this pass

- Recommendation #2 (unique C++ temp names) was applied and validated; all
  other recommendations remain open (notably #1, the single-instance lock, which
  is the remaining cause of concurrent-run collisions on the shared `build\`
  tree). The auto-memory build/test note was corrected.
- `__audit_tmp.txt` was deleted (stale 2026-05-29 root scratch).
- Line numbers cited from the static cross-check may drift; symbols are
  authoritative.
