# Testing/Script Stack and Runtime Hot-Path Simplification Review — 2026-06-10

Status: **applied 2026-06-10** (same day, maintainer-directed; one commit per
finding group — see the per-finding "Applied" notes and `git log`). Originally
two read-only sweeps (test/script stack; runtime hot paths) with every
load-bearing claim re-verified against the cited code. Behavior-preserving
refactors do not require a feature spec (`AGENTS.md` §Feature Intake Gate).
The application pass was executed as a 24-agent migration workflow over
disjoint files plus 4 adversarial verification lenses over the complete diff
(0 blockers), then gated by `Test-LocalCI.ps1 -KeepBuildDir` (CTest 12/12,
hermetic Python lane 133 tests). Prior completed passes were read first and are not re-proposed:
`docs/testing-harness-evaluation-2026-06-06.md` (all 7 recommendations applied),
`docs/archive/implemented-plans/SCRIPT_STACK_REVIEW.md`, `docs/archive/implemented-plans/CONTROL_SIMPLIFICATION_TARGETS.md`,
`docs/archive/build-optimization-results.md`.

**Overall assessment:** no critical findings. The 250 ms loop is healthy
(disabled-baseline `loop_slip_ms` p95 1.56 ms, ~6.5 overrun rows/h, 2026-06-10
capture-runbook §1), the hot path already caches its snapshot index per tick
and reuses thread-local buffers, and the harness was consolidated in the
2026-06-06 pass. What remains is duplication cleanup in the test files and two
micro items recorded mainly so future sweeps do not re-litigate them.

## Part 1 — tests and scripts

### TS-1 (verified) — Expect-helper boilerplate duplicated across 10 C++ test files

Every file under `tests/cpp/` defines its own `g_failures` +
`ExpectTrue`/`ExpectNear` (and several also `ExpectFalse` /
`ExpectThrowsContaining`): verified 10 of 11 files define
`void ExpectTrue(bool` locally (all except `csv_rows_tests.cpp`, which uses
its own variants). `tests/cpp/power_anticipation_tests.cpp` (added 2026-06-10)
follows the same pattern, so the duplication grows with each new test.

- Proposal: one `tests/cpp/test_helpers.h` with the counter + helpers;
  each test includes it. The helpers are identical in semantics; the only
  variance is `boost_stage_tests.cpp:28-30` treating NaN==NaN as pass, which
  the shared `ExpectNear` should adopt (strictly more lenient; no existing
  assertion changes outcome).
- Estimated saving: ~120 LOC now, plus ~15 LOC per future test.
- Risk: low. Pure extraction; CTest names/registration unchanged.
- **Applied 2026-06-10**, with one deliberate deviation beyond this finding's
  "strictly more lenient" wording: the shared `ExpectNear` makes NaN-vs-number
  FAIL (every historical per-file copy silently passed it, because
  `fabs(NaN - x) > tol` is false). Documented in the header; verified
  non-outcome-changing on the current suite (CTest 12/12). Failure-message
  text is standardized (boost_stage's ` diff` suffix dropped — stderr text
  only). `csv_rows_tests.cpp` keeps its own `g_failures` + field matchers by
  design and does not include the header.

### TS-2 (verified) — `UniqueTempSuffix` duplicated in 2 C++ test files

`tests/cpp/control_loop_config_tests.cpp` and
`tests/cpp/channel_write_tests.cpp` both implement the per-process
random-salted temp suffix introduced by the 2026-06-06 harness pass (3
occurrences each). Belongs in the same shared header as TS-1.

**Applied 2026-06-10**: both copies removed; the single definition lives in
`tests/cpp/test_helpers.h`.

### TS-3 (verified) — Windows-gate `setUpClass` boilerplate in 10 Python test files

10 files repeat the same 4-line `setUpClass` (platform skip +
`_ensure_release_build()`); verified via
`raise unittest.SkipTest("Windows-only` (11 occurrences / 10 files).

- Proposal: a `WindowsExeTestCase(unittest.TestCase)` base class (or a
  module-level helper) in `tests/helpers.py`; subclasses drop their
  `setUpClass`. `tests/test_power_lead.py` needs no gate (pure math) and is
  the counterexample that the gate should stay opt-in, not implicit in
  discovery.
- Estimated saving: ~40 LOC.
- Risk: low; no logic change.
- **Applied 2026-06-10** to 11 files / 12 classes — one more file than this
  finding counted: `test_analyzer.py`'s gate escaped the original grep
  because its skip message read "svg-mb-control.exe is Windows-only"; it now
  reports the base class's "Windows-only repo" (skip-reason text only). The
  base class is exported through the module's dynamic `__all__` into
  star-importing modules; it must never gain `test_*` methods (constraint
  documented in its docstring — a test there would run once per importing
  module). Discovery collects the same 133 cases before and after.

### TS-R1 (rejected) — Build-Release.ps1 "duplicate hashing" is deliberate verification

The sweep flagged `scripts/Build-Release.ps1:356-375` for hashing each exe
twice (build location + dist copy). Re-read: the two hashes are compared and a
mismatch throws — it is a copy-integrity check, not redundant work. Computing
one hash would delete the verification. No change.

### TS-R2 (checked, fine as-is) — recorded so future sweeps skip them

- `RuntimeProbe` vs `StagedControlApp` (tests/helpers.py): different roles,
  merging would overload one class.
- Bespoke C++ assert style vs GoogleTest/Catch2: current style is
  dependency-free and already factored at the CMake level
  (`svg_mb_control_add_core_test`); a framework adds a dependency for no
  behavioral gain.
- Serial `unittest discover` (~120 tests): parallelizing adds process
  overhead and debugging cost at this scale; measure first if wall time
  becomes a complaint.
- Flat config-builder functions in `tests/helpers.py`: deliberately flat;
  a factory class adds ceremony without clearer intent.

## Part 2 — runtime / hot paths

### HP-1 (verified, test-only) — 3-arg `BuildControlLoopCsvRow` overload rebuilds the snapshot index

`src/runtime/runtime_csv_rows.cpp:698-707` rebuilds a `RuntimeSnapshotIndex`
per call. Verified call sites: the live loop does NOT use it —
`src/control/tick_runner.cpp:371` passes the cached
`state.runtime_snapshot_index` to the 4-arg overload. The only caller is
`tests/cpp/csv_rows_tests.cpp:245`. So this is not a runtime cost; it is a
convenience overload whose existence invites an uncached call from future
code.

- Proposal: delete the 3-arg overload (header + impl, ~12 LOC) and build the
  index explicitly in the one test call site (~3 LOC).
- Risk: low; compile-checked by the test itself.
- **Applied 2026-06-10**: overload deleted from `runtime_csv_rows.h`/`.cpp`;
  `csv_rows_tests.cpp` now builds the index explicitly (its existing
  `runtime_snapshot.h` include provides the type); repo-wide grep confirms
  zero remaining 3-arg callers and `tick_runner.cpp:371` (cached index,
  4-arg) is untouched.

### HP-2 (verified, negligible) — status-JSON boost keys built per write

`src/runtime/runtime_status.cpp:32-37` builds the four
`last_<stage>_boost_pct` key strings per channel on every status write. The
write is rate-limited to every 10 ticks (2.5 s), so this is ~24 small string
constructions per 2.5 s across 6 channels — measurable in lines of code, not
in runtime. The in-code comment explains the table-driven contract is the
point. Optional: precompute the four keys once (static table built from
`kBoostStageSpecs` at first use). Worth doing only as a drive-by.

**Applied 2026-06-10** (the drive-by condition was met — `runtime_status.cpp`
was open as part of this pass): a function-local static `kBoostKeys` array is
built once from `kBoostStageSpecs` (magic-static thread-safe init); emitted
JSON keys are byte-identical in the same order; `<utility>` dropped with the
removed `std::move`.

### HP-3 (checked, fine as-is) — recorded so future sweeps skip them

- Per-tick CSV row built via `std::ostringstream` + flush-per-write:
  documented intentional (`tick_runner.cpp` comment, RUNTIME_LOGGING docs).
- Snapshot vectors cleared-with-capacity and reused
  (`src/platform/direct_runtime_snapshot.cpp:98-99,180-181`).
- Hot loop caches `RuntimeSnapshotIndex` once per tick
  (`tick_runner.cpp:160`).
- `FindByKey` linear search (`runtime_csv_rows.cpp:37-46`): O(n) over n ≤ ~10
  fans/sensors per row; an index/hash adds complexity for no measurable gain
  at current sizes.
- `response_source` attribution strings (`channel_evaluator.cpp:307-336`):
  move-based appends; the strings must exist because they are published to
  CSV/status. The two copies at lines 334-335 (state + evaluation) are
  per-channel-per-tick small-string copies — below the churn threshold.
- `BuildAmdSensorSummary` uses a `thread_local` buffer
  (`runtime_csv_rows.cpp:430`): already allocation-free per tick.
- Boost-stage and low-band math: no allocations, no string work.

## Execution record (2026-06-10)

All four groups were applied the same day in the suggested order
(TS-1+TS-2, TS-3, HP-1, HP-2 as the permitted drive-by), one commit per
group. Validation: `Test-LocalCI.ps1 -KeepBuildDir` green on the final tree —
build clean, CTest 12/12 (same targets), hermetic Python lane 133 tests (same
collection count before/after). Adversarial diff verification (4 independent
lenses: C++ assertion semantics/ODR, Python migration/imports, runtime
byte-identity, completeness) reported 0 blockers; the minor accepted
deviations are recorded in the per-finding "Applied" notes above.
