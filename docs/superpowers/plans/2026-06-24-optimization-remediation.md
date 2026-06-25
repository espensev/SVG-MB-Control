# Optimization Remediation Plan - 2026-06-24

Status: Proposed
Source review: [docs/reviews/2026-06-24-optimization-claims-code-review.md](../../reviews/2026-06-24-optimization-claims-code-review.md)

This plan addresses only the claims that are worth acting on from the code
review. It favors small correctness hardening and measured performance work
over speculative hot-path rewrites.

## Decision Summary

- Do now: replace CPUID aliasing with `std::memcpy`.
- Do now: tighten the parser paths that currently accept trailing junk, with
  tests around existing fallback behavior.
- Do now: add selective `[[nodiscard]]` and `noexcept` annotations where they
  improve compiler help without changing semantics.
- Measure before changing: analyzer CSV parsing and any allocation-heavy parser
  path that runs outside simulation setup.
- Defer: snapshot string interning or shared ownership changes until runtime
  evidence shows copy cost in the sample loop.
- Defer: default LTO, PGO, sanitizer, and static-analysis build changes until
  they are introduced as opt-in tooling lanes and validated against the repo's
  documented build workflow.

## Scope Gates

- Behavior-preserving C++ cleanup does not need a feature spec.
- Tightening malformed input handling is treated as a defect fix when the
  intended behavior is already "reject invalid input and use the documented
  fallback." Add tests before or with the change.
- Any new CLI surface, runtime schema/status/log field, shipped config
  behavior, or control behavior expansion must first map to an accepted,
  implementation-authorized feature spec.
- Do not use live runtime interaction for this plan. Validation should use the
  documented local build and test workflows.

## Package 1 - Tiny Correctness Hardening

Goal: remove undefined-behavior risk without changing AMD detection behavior.

Tasks:

- In `src/hardware/amd_reader.cpp`, replace the `reinterpret_cast<int*>`
  writes into the CPUID vendor buffer with `std::memcpy`.
- Keep the vendor comparison behavior and resulting strings unchanged.
- Avoid extracting a broad CPUID abstraction unless tests or later work need it.

Validation:

- Run `git diff --check`.
- Run `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`.

Exit criteria:

- No `reinterpret_cast<int*>` writes remain in `DetectAmdCpu`.
- Existing AMD detection behavior is unchanged.

## Package 2 - Parser Correctness and Allocation Cleanup

Goal: make malformed numeric input handling explicit and stop accepting
trailing garbage where the current code uses partial `std::stod` or
`std::stoul` parses.

Implemented contract (`src/platform/numeric_parse.h`): the strict `from_chars`
helper requires a base-10 token and rejects — beyond trailing junk — a leading
`+`, a `0x`/hex-float prefix, and non-finite `inf`/`nan`. This is a deliberate
narrowing versus the `std::stod`/`std::stoul` it replaces (all affected callers
are `SVG_MB_CONTROL_SIM_*` knobs), pinned by `numeric_parse_tests.cpp`. The
wide-char operator-arg sites (`analyze_cli.cpp`, `calibration.cpp`) keep
`std::stod`/`std::stoul` plus a consumed-count check, so they retain the broader
accept-set.

Tasks:

- Add or reuse a small strict numeric parsing helper for string views.
- Update `TryParseDoubleEnv` and `ParseDoubleSequence` so they parse without
  temporary token strings and reject trailing non-whitespace data.
- Preserve existing fallback behavior:
  - malformed single-value AMD simulation input should fall back to the normal
    simulated/default source path;
  - malformed AMD sequence input should not create a partial sequence.
- Audit and tighten the operator-facing numeric parsing sites called out by the
  review:
  - `src/analyze/analyze_cli.cpp`;
  - `src/control/calibration.cpp`;
  - simulator/env helpers that parse numeric environment values.
- Keep CSV row parsing behavior aligned with the existing strict parser in
  `src/analyze/analyze_csv.cpp`.

Tests:

- Add focused tests for accepted values, rejected trailing junk, empty tokens,
  whitespace, and fallback behavior.
- Prefer existing executable or Python test harness entry points. Do not add a
  new standalone parser test harness unless the current test shape cannot
  reach the behavior cleanly.

Validation:

- Run the narrow parser/analyzer tests if available.
- Run `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`.

Exit criteria:

- Numeric parsing semantics are consistent at the audited sites.
- Tests cover the malformed-input cases that previously could be accepted by
  partial parses.

## Package 3 - Result-Handling Compiler Help

Goal: make ignored low-level results harder to miss without creating noisy or
misleading annotations.

Tasks:

- Mark status/result-returning functions or result types `[[nodiscard]]` where
  ignoring the value is almost certainly a bug.
- Start with PawnIO/SIO/device-read and fan-read/write result surfaces rather
  than broad annotations on every helper.
- Mark trivial non-allocating accessors such as `available()` as `noexcept`
  where the implementation genuinely cannot throw.
- Do not mark `init_warning()` `noexcept`; returning `std::string` by value can
  allocate.

Validation:

- Build with the documented local workflow and fix any newly surfaced ignored
  result warnings intentionally.
- Run `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`.

Exit criteria:

- Important hardware/status results get compiler assistance.
- Added annotations do not require suppressing legitimate warnings.

## Package 4 - Driver and Fallback Test Seams

Goal: cover fallback and status translation behavior that is currently mostly
verified by code inspection.

Tasks:

- Identify the smallest pure helpers worth extracting for tests, such as status
  translation, retry classification, and CPUID vendor decoding.
- Add tests for not-found, access-denied, transient failure, and normal success
  paths where a hardware dependency can be replaced with a pure seam.
- Keep real driver loading and live device interaction out of unit tests.

Validation:

- Run the new focused tests.
- Run `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`.

Exit criteria:

- Fallback paths have deterministic tests that do not require PawnIO, NVAPI, or
  administrator hardware access.

## Package 5 - Measured Analyzer CSV Performance

Goal: optimize the parser that is most likely to process large input, but only
  after recording a baseline.

Tasks:

- Pick a representative CSV ingest/analyze workflow that uses repo-owned test
  or fixture data, not raw live captures committed to source.
- Record a baseline wall-clock measurement through the documented analyzer
  entry point.
- Profile or inspect allocation-heavy row paths, especially repeated field-name
  construction and per-row lookup work.
- Apply small behavior-preserving optimizations such as reserving vectors,
  precomputing column indexes, and reducing repeated string construction.

Validation:

- Run analyzer correctness tests.
- Re-run the same benchmark command and record before/after numbers in the
  implementation note or follow-up review artifact.
- Run `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` if C++ changes are included.

Exit criteria:

- Performance claims are backed by recorded before/after measurements.
- Analyzer output remains behaviorally identical on the chosen fixtures.

## Package 6 - Optional Build and Static-Analysis Lanes

Goal: add useful toolchain checks without destabilizing the default release
workflow.

Tasks:

- Add opt-in build options before changing default packaging behavior.
- Consider a guarded MSVC IPO/LTO option after confirming generated artifacts
  and build time.
- Consider opt-in MSVC `/analyze`, clang-tidy, and sanitizer lanes only when
  the required tool is present and the documented workflow remains the primary
  entry point.
- Keep PGO as a later manual experiment that uses representative control-loop
  and analyzer workloads.

Validation:

- Validate each lane independently with the documented scripts or narrow
  documented script options.
- Do not require optional tooling for normal local CI unless the repo is ready
  to carry that support burden.

Exit criteria:

- Optional analysis/performance lanes are discoverable, documented, and do not
  change default release behavior unexpectedly.

## Explicit Non-Goals

- Do not change fan-control behavior, runtime cadence, live scheduled tasks, or
  hardware write paths as part of this plan.
- Do not intern or share snapshot strings without runtime timing evidence.
- Do not enable `/GL`, `/LTCG`, PGO, sanitizers, or clang-tidy by default in the
  release workflow during the first pass.
- Do not introduce sibling-repo dependencies or external bridge code paths.
