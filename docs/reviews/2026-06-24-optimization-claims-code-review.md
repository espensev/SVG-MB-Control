# Optimization Claims vs Code Review - 2026-06-24

## Verdict

The claims are directionally useful, but several are broader than the current
code supports.

The concrete, code-backed cleanup candidates are:

- replace the CPUID `reinterpret_cast<int*>` writes in `DetectAmdCpu` with
  `std::memcpy`;
- tighten the AMD simulation/env parsers if that path matters, ideally while
  also rejecting trailing junk that `std::stod` currently accepts;
- add `[[nodiscard]]` to low-level status-returning helpers and expand tests for
  PawnIO/SIO status translation and CPUID decode;
- add explicit optional build lanes for static analysis/sanitizers/LTO if those
  are desired.

The "hot-path parsing" claim is overstated for the real AMD hardware read path:
the named AMD parsers are simulation/env setup or simulation per-tick paths, not
the normal PawnIO Tctl/CCD sampling path. The stronger parser opportunity in the
repo is the offline analyzer CSV parser, which already has a `from_chars` double
fast path but still materializes fields as `std::string` per row and has a
larger header/field lookup optimization outstanding.

No live runtime interaction was performed for this review.

## Findings

### 1. Real correctness cleanup: CPUID buffer aliasing

Claim 2 is valid.

`src/hardware/amd_reader.cpp:336-339` fills `char vendor[13]` by writing through
`reinterpret_cast<int*>(vendor)`. `src/hardware/amd_reader.cpp:366-368` passes
`brand + offset` reinterpreted as `int*` directly to `__cpuid`. On MSVC/x64 this
is unlikely to fail in practice, and the path is startup-only, but it is still
unnecessarily dependent on alignment and aliasing assumptions.

Recommended change:

- keep `int cpu_info[4]` as the `__cpuid` output;
- `std::memcpy(vendor + 0, &cpu_info[1], sizeof(int))`, then EBX/EDX/ECX order;
- for brand leaves, call `__cpuid(cpu_info, leaf)` and `std::memcpy` the full
  16-byte block into `brand + offset`.

This is behavior-preserving and low risk. It also makes the AMD detection code
easier to test if the family/model decode is later extracted.

### 2. Parser performance claim is partly true, but not in the real AMD hot path

Claim 1 is partly valid and partly overstated.

In `src/hardware/amd_reader.cpp:89-105`, `TryParseDoubleEnv` uses `_dupenv_s`,
copies the value into `std::string`, then calls `std::stod`. This can run per
simulated AMD tick at `src/hardware/amd_reader.cpp:1078-1081` when
`SVG_MB_CONTROL_SIM_DIRECT_AMD_MODE=enabled` and no fixed sequence is provided.

In `src/hardware/amd_reader.cpp:108-132`, `ParseDoubleSequence` builds a
temporary `std::string` for every token and calls `std::stod`, but that function
is only called from the `AmdReader` constructor in simulation setup at
`src/hardware/amd_reader.cpp:996-1001`.

The real hardware sample path does not call either parser. Real sampling copies
stable strings at `src/hardware/amd_reader.cpp:1059-1060`, samples package/cycle
evidence at `src/hardware/amd_reader.cpp:1093-1101`, then takes one
`Global\Access_PCI` lock and reads SMN registers at
`src/hardware/amd_reader.cpp:1103-1139`.

So the recommended parser fix is still reasonable, but the impact should be
described as simulation/env cleanup and parser correctness, not as a primary
control-loop performance win on deployed hardware.

### 3. Parser correctness is a hidden issue in several `std::stod` sites

This was not in the original claim, but it matters if the parser work is done.

`TryParseDoubleEnv` and `ParseDoubleSequence` call `std::stod` without checking
the parsed character count. That means values with a valid numeric prefix and
trailing junk can be accepted instead of rejected. The same pattern appears in
operator/setup parsing:

- `src/analyze/analyze_cli.cpp:254`, `:263`, and `:272` parse report options
  with `std::stod(std::wstring(...))` and no consumed-count check;
- `src/control/calibration.cpp:70-72` parses `duty_pct:hold_ms` tokens with
  `std::stod` / `std::stoul` on substrings and no consumed-count check;
- simulation/env helpers in `src/hardware/simulated_fan_writer.cpp:30` and
  `:58`, `src/hardware/gpu_reader.cpp:103-153`, and
  `src/platform/direct_runtime_snapshot.cpp:43` use the same broad pattern.

By contrast, `src/app/app_args.cpp:51-61` already checks full consumption for
`ParseDoubleArg`, and `src/analyze/analyze_csv.cpp:27-45` uses `from_chars` first
and checks full consumption on the `std::stod` fallback.

If these parsers are changed, add focused tests for full-token acceptance and
rejection. A pure `from_chars`/`strtod` replacement that silently changes
accepted inputs without tests would be riskier than the current performance
problem.

### 4. Snapshot buffer reuse is already in place; remaining copies are stable-string copies

Claim 3 is accurate about copies, but the current code has already addressed the
larger vector-buffer problem.

`AmdReader::Sample` reuses `sample_buffer_` and clears fields at
`src/hardware/amd_reader.cpp:1029-1038`; the comment explicitly notes that
`clear()` retains the samples vector capacity. The steady-state path then
assigns `cpu_name` and `transport_path` every tick at
`src/hardware/amd_reader.cpp:1059-1060`, and pushes `AmdTemperatureSample`
objects with string labels at `src/hardware/amd_reader.cpp:1126-1131` and
`:1149-1154`.

`src/platform/direct_runtime_snapshot.cpp:204-212` also clears but reuses the
output snapshot vectors. `MergeAmdTelemetry` reserves to the actual sample count
before copying AMD sensors at `src/platform/direct_runtime_snapshot.cpp:107-111`.

That means the remaining cost is not repeated vector allocation under normal
capacity retention. It is:

- assigning/copying stable `std::string` fields every tick;
- constructing/copying sensor label strings every tick;
- likely-small fixed push counts (Tctl plus up to CCD sensors).

Keep the owned-snapshot contract unless profiling says otherwise. A cheap
cleanup would be `snapshot.samples.reserve(kMaxCcds + 1u)` once after construction
or before the first push, but that is unlikely to be a major win by itself.

### 5. Vector growth/push patterns are mostly not a live-loop issue

Claim 4 is low priority.

The live snapshot paths mostly reserve or retain buffers:

- `AmdReader::Sample` clears a reused vector at `src/hardware/amd_reader.cpp:1035`;
- `SampleDirectRuntimeSnapshot` clears reused output vectors at
  `src/platform/direct_runtime_snapshot.cpp:211-212`;
- `MergeAmdTelemetry` reserves before copying at
  `src/platform/direct_runtime_snapshot.cpp:107-111`;
- `ParseRuntimeSnapshotJson` reserves for JSON arrays at
  `src/runtime/runtime_snapshot.cpp:105-129`;
- SIO read paths reserve output vectors for bulk fan/voltage/temperature reads
  at `src/hardware/sio_fan_writer.cpp:174`, `:182`, `:228`, and `:253`.

The analyzer CSV parser does not reserve `fields` in `ParseCsvLine`
(`src/analyze/analyze_csv.cpp:177-215`), and `ParseControlLoopCsv` appends rows
without a known row count at `src/analyze/analyze_csv.cpp:437-438`. That can
matter for multi-hour offline captures, but it is not control-loop latency.

Use `reserve` / `emplace_back` opportunistically where it preserves clarity, but
do not treat this as a high-priority thermal-control fix.

### 6. PCI mutex handling is good in the AMD reader

Claim 5 checks out for the named resource.

The AMD reader opens or creates `Global\Access_PCI` at
`src/hardware/amd_reader.cpp:206-211`, wraps acquisition/release in `PciMutexLock`
at `src/hardware/amd_reader.cpp:214-241`, and takes one lock around the whole
Tctl plus CCD sequence at `src/hardware/amd_reader.cpp:1103-1111`. `ReadSmnLocked`
documents that callers must already hold the mutex at
`src/hardware/amd_reader.cpp:599-605`.

The broader source scan found no other `Global\Access_PCI` user in this repo.
`DeviceIoControl` appears in the AMD/PawnIO paths only:

- `src/hardware/amd_reader.cpp:270-285` for PawnIO function execution;
- `src/hardware/pawnio_binary.cpp:305-311` for module loading.

The Super I/O path uses the vendored SVG-MB-SIO layer and documents a separate
ISA bus mutex in `docs/BUILD_TARGETS_AND_DEPENDENCIES.md:188-194`. I did not
find a per-read churn problem around `Global\Access_PCI`.

### 7. `noexcept`/`[[nodiscard]]` usage is sparse

Claims 6 and 7 are valid as code-quality hardening.

Only a small number of platform helpers currently use `noexcept`, such as
`RuntimeSingletonLock::held()` in `src/platform/runtime_singleton.h:31`.
`AmdReader::available()` and `GpuReader::available()` are plain `const`
functions (`src/hardware/amd_reader.h:81`, `src/hardware/gpu_reader.h:136`) and
their implementations do not throw under normal assumptions. `init_warning()`
returns `std::string`, so marking it `noexcept` would be wrong because it can
allocate.

More valuable is `[[nodiscard]]` on status/result functions. There is no current
`[[nodiscard]]` usage in `src/`. Status-returning helpers include:

- `StatusFromWin32Error`, `MapPawnIoStatus`, `ExecutePawnIo`,
  `ReadAllowlistedMsr`, `ReadAllowlistedCycleMsr`, and `ReadSmnLocked` in
  `src/hardware/amd_reader.cpp`;
- `LoadPawnIoBinary` in `src/hardware/pawnio_binary.h:53-57`;
- `FanWriteResult` methods in `src/hardware/fan_writer.h`.

Most current call sites check the return values, but the compiler is not helping
keep it that way.

### 8. Low-level status translation exists, but test seams are incomplete

Claim 7 and claim 10 are partly already implemented and partly valid.

Current code maps not-found/access-denied conditions explicitly:

- AMD reader maps Win32 errors to `Status::no_device` /
  `Status::access_denied` at `src/hardware/amd_reader.cpp:135-147`;
- PawnIO binary loading maps the same classes at
  `src/hardware/pawnio_binary.cpp:60-72`;
- SIO status translation maps `no_device` and `access_denied` to
  `FanWriteError::kUnavailable` and timeout to `kTimedOut` at
  `src/hardware/sio_fan_writer.cpp:57-86`.

The repo also documents the fallback posture: `docs/BUILD_TARGETS_AND_DEPENDENCIES.md:153-210`
states that the app assumes PawnIO is already installed, records
`access_denied` / `no_device` warnings, continues without AMD CPU telemetry,
does not install/start/restart the driver, and has no remediation path.

The remaining issue is testability. `tests/cpp/pawnio_binary_tests.cpp` covers
hash helpers, spec population, unknown path resolution, and status-string
coverage, but it does not simulate `LoadPawnIoBinary` device IOCTL outcomes.
`docs/testing-harness-evaluation-2026-06-06.md:35-37` and
`:291-295` already record open gaps for `LoadPawnIoBinary`, SIO status/retry,
CPUID decode, and `ParseTickRow`.

### 9. Build/CI optimization tooling is not currently wired

Claim 8 is mostly valid, with one correction.

The documented release path configures CMake/Ninja Release:

- `CMakePresets.json:27-32` defines the `x64-release` preset with
  `CMAKE_BUILD_TYPE=Release`;
- `scripts/Build-Release.ps1:303-335` configures and builds that preset with
  Ninja and `--parallel`.

For MSVC, Release implies optimized compiler defaults such as `/O2` through
CMake's standard release flags. The repo also adds `/W4 /permissive- /MP` for
normal targets at `CMakeLists.txt:126-140`.

I did not find explicit `/GL`, `/LTCG`, CMake interprocedural optimization,
`clang-tidy`, MSVC `/analyze`, ASan, UBSan, or CodeQL/static-analysis workflow
wiring in current CMake, scripts, or GitHub Actions. The CI workflow runs only
the documented local CI entrypoint at `.github/workflows/ci-windows.yml:28-58`.

Recommendation: add these as opt-in lanes before making them default. `/GL` and
`/LTCG` can change build time and linker behavior, and sanitizer availability on
MSVC/Windows is narrower than on Clang/Linux.

### 10. Microbenchmark claims need measurement before prioritization

Claim 9 is directionally useful but not currently backed by repo benchmarks.

The repo has runtime-analysis scripts and C++/Python tests, but I did not find a
microbenchmark target for `ParseDoubleSequence`, analyzer CSV parsing, or control
tick parser costs. `docs/discovery-control-math-performance.md:134-157` already
says the analyzer double fast path is partially implemented, the header parse
plan remains, and parser/report opportunities should be measured against a
representative archived CSV before claiming throughput gains.

Before adding a benchmark harness, decide whether the benchmark target is:

- simulation env parsing (`TryParseDoubleEnv`, low deployment impact);
- analyzer CSV ingest (`ParseCsvLine`, field lookup, `TryParseDouble`, likely
  higher payoff for large captures);
- live per-tick snapshot/string-copy overhead (`AmdReader::Sample` and
  `SampleDirectRuntimeSnapshot`, only useful if runtime timing evidence points
  there).

## Claim-by-Claim Status

| Claim | Verdict | Notes |
|---|---|---|
| 1. Hot-path parsing/allocations | Partly valid, impact overstated | AMD named parsers are sim/setup paths. Analyzer CSV parsing is the stronger offline target. Several `std::stod` sites also accept trailing junk today. |
| 2. CPUID buffer aliasing | Valid | Startup-only but real cleanup. Use `std::memcpy`. |
| 3. Reused snapshot buffer/copies | Mostly already mitigated | Buffer reuse is present. Stable string/label copies remain. Profile before changing snapshot ownership. |
| 4. Vector growth/push patterns | Low priority | Many live paths reserve or retain capacity. Analyzer CSV has room for offline optimization. |
| 5. Mutex strategy | Good | AMD SMN path takes one `Global\Access_PCI` lock per sample. No other repo user found. |
| 6. Accessors/attributes | Valid low priority | `available()` can be `noexcept`; `init_warning()` should not be. `[[nodiscard]]` is more useful. |
| 7. Status handling | Partly valid | Mapping exists; compiler enforcement and deeper simulated tests are missing. |
| 8. Build/toolchain/CI | Mostly valid | Release build is already optimized by CMake/MSVC defaults, but no explicit LTO/static-analysis/sanitizer lanes are wired. |
| 9. Safety/testing | Valid | Existing tests cover many pure helpers, but not driver IOCTL simulations, SIO retry/status seams, parser edge cases, or microbenchmarks. |
| 10. Third-party fallback | Mostly already covered | Fallback/diagnostics are explicit; remediation is intentionally external. Tests remain thin for live-driver failure simulation. |

## Safe Implementation Order

1. Fix CPUID buffer construction with `std::memcpy`.
2. Add parser edge-case tests first, then replace the AMD simulation parsers with
   full-consumption `from_chars` or `strtod`-based helpers.
3. Add `[[nodiscard]]` to `PawnIoStatus`, internal `Status`, and
   `FanWriteResult` surfaces where ignored results would be a bug.
4. Add focused tests for `analyze_cli` / calibration parser trailing-junk
   rejection if changing those parser semantics.
5. Measure analyzer CSV ingest against a representative archived CSV before
   refactoring `ParseCsvLine` / header lookup.
6. Add optional CI lanes for clang-tidy/MSVC `/analyze` and sanitizer builds
   separately from the default release/local-CI lane.
