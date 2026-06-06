# Path Notes

A running, human-readable log of the repo's path: what was **done**, what was
**fixed/repaired**, what was **added**, and **ideas** worth keeping. Newest entry
at the top.

This is a curated journal, not a system of record. Authoritative sources stay:
`git log` (exact changes), `docs/STRUCTURE_AND_STABILITY.md` §Migration Order
(completed structural moves), `docs/features/` (specs + `REQ-*`), and `.remember/`
(per-session buffer). Entries here cite commit hashes or files so each claim is
checkable.

## Conventions

- One dated section per working day (`## YYYY-MM-DD`). Append newest at the top.
- Tag each line: **Done** (shipped/committed), **Fixed** (a defect or gap
  closed), **Added** (new capability/doc/test), **Idea** (not a commitment).
- Keep `CLAUDE.md` doctrine: claims grounded in a commit/file/test; `is`/`was`
  for current/historical facts; ideas labeled as ideas; no vague adjectives
  without a field, number, or test.
- An **Idea** is not authorized work. It becomes work only via a feature spec
  (`docs/features/`) or an explicit go-ahead.

---

## 2026-06-06

- **Added** — `docs/testing-harness-evaluation-2026-06-06.md`: a findings
  snapshot of the local CI test harness (`scripts/Test-LocalCI.ps1` →
  `Build-Release.ps1`, both test lanes). Verdict: green and live-loop-safe (CTest
  8/8, hermetic 114/114, 0 skips on this Windows host); names the real weaknesses
  — shared mutable state across runs (single `build/` tree, fixed-name `%TEMP%`
  files), several skip-rather-than-fail paths, and 5 adversarially-verified
  coverage gaps in the real-hardware decode/translate path (one High:
  `amd_reader.cpp` `DecodeTctl`/`DecodeCcdTemp` is structurally unreachable from
  the harness — no `SIM_AMD_*_RAW` hook). Labeled a snapshot, not a maintained
  contract; `git log`/`README.md`/tests stay authoritative.
- **Fixed** — flaky concurrent-run collision in the C++ test lane: two
  `Test-LocalCI` runs shared fixed-name `%TEMP%` artifacts
  (`svg_mb_control_config_test_<N>.json` and
  `svg_mb_control_channel_write_tests_<name>`), so one run could truncate a file
  mid-read in the other and fail `svg_mb_control_loop_config_tests` with "parse
  error ... empty input". `TempJsonFile`
  (`tests/cpp/control_loop_config_tests.cpp`) and `MakeTempHome`
  (`tests/cpp/channel_write_tests.cpp`) now carry a per-process `random_device`
  salt + counter (`UniqueTempSuffix`); the misleading "distinct per concurrent
  run" comments are corrected. Re-validated: CTest 8/8, hermetic 114/114, exit 0
  (harness-eval rec 2).
- **Done** — applied all remaining harness-eval recommendations (rec 1, 3–7;
  rec 2 is the temp-name fix above), each validated through
  `Test-LocalCI.ps1 -KeepBuildDir` and committed separately:
  - **Fixed** — rec 1 single-instance lock on `Test-LocalCI`: a per-repo
    exclusive lock file in the system temp dir; a second concurrent run refuses
    (exit 75) or queues with `-Wait`, removing the shared-`build/` and `%TEMP%`
    collisions at the source (`470fc53`).
  - **Fixed** — rec 3 fail-loud `[2/11]` clean (opt-in `-Strict` on
    `Remove-DirectoryIfExists`, so a locked tree fails the build with exit 1
    instead of building against stale artifacts) plus CTest `--no-tests=error`
    (`9a99834`).
  - **Added** — rec 4 closes the §5 High coverage gap: `DecodeTctl` /
    `DecodeCcdTemp` / `SelectCcdLayout` extracted to `src/hardware/amd_decode.h`
    and unit-tested by a new 9th CTest target `svg_mb_control_amd_decode_tests`
    (`547eae6`). The CPUID family/model decode in `DetectAmdCpu` stays untested.
  - **Fixed** — rec 5 doc drift: README DB schema `7`→`8` + the two
    `low_band_*_boost_pct` columns, CTest lane documented, `-SkipTests`
    corrected to both lanes; `RUNTIME_LOGGING_AND_EVALUATION.md` date advanced
    (`cd3f672`).
  - **Fixed** — rec 6 `.gitignore` broadened to `/__*` and `*_run.log`
    (`55b59f7`).
  - **Added** — rec 7 defense-in-depth: the test kill helper
    (`_terminate_svg_processes`) now also filters on `.Path` under the staged
    root, so a recycled PID matching a live `release\` instance is not killed
    (`e46a6bc`). Filter logic verified; not exercised end-to-end (fallback path).
  Full-sweep re-validation: CTest 9/9, hermetic 114/114, exit 0.
- **Fixed** — recovery-gap remediation 3 (the compound cell): a sensor-safe
  (safe-mode) command now bypasses an open write-failure breaker so a
  thermal-safety write reaches the hardware instead of being silently dropped
  (new `ChannelEvaluation::safety_override`, set on the safe-mode path in
  `channel_evaluator.cpp`, honored at the breaker gate in `channel_write.cpp`).
  Normal writes still respect the breaker. Regression test
  `tests/cpp/channel_write_tests.cpp` (CTest 8/8, pytest 114/114); docs updated
  (`CONTROL_PIPELINE_MATH.md`, `CONTROL_LOOP.md`,
  `discovery-recovery-gap-audit-2026-06-04.md`).
- **Done** — `FEAT-0001` (hot-swap write policy) promoted `Draft` → `Accepted`:
  the build-then-swap design decision
  (`docs/write-policy-hotswap-decision-2026-06-03.md`, Option A) accepted by the
  maintainer, so promotion gate 3 is cleared. Not yet implemented — the spec §5
  behavior stays proposed until it is built and the verification log (§14) is
  filled.

## 2026-06-03

- **Added** — `docs/features/` spec-before-build system in active use:
  `FEAT-0001` (hot-swap write policy, `REQ-WRITEPOLICY-*`), `FEAT-0002`
  (CPU-settings evidence logger, `REQ-CPUSETTINGS-*`), `FEAT-0003`
  (selectable control-law profile, `REQ-PROFILE-*`), each with the template's
  promotion-gate checklist.
- **Added** — `docs/BUILD_TARGETS_AND_DEPENDENCIES.md`: executables
  (`svg-mb-control.exe`, `svg-mb-control-task-runner.exe`), the scheduled-task
  runtime processes, vendored libraries, and a Drivers and Hardware Access
  section (single PawnIO kernel driver; AMD telemetry via SMN reads not MSR;
  Super I/O via kernel-side LPC port I/O; a clearly-labeled VBS/HVCI assessment).
  `AGENTS.md` navigation points at it.
- **Added** — decision records: `docs/write-policy-hotswap-decision-2026-06-03.md`
  (FEAT-0001: build-then-swap the `FanWriter`) and
  `docs/profile-hot-swap-decision-2026-06-03.md` (FEAT-0003: control-law seam,
  full state decouple, selectable PID feed-forward, shadow-default live gate).
- **Done** — readiness review for a swappable control law. Finding: today the
  per-channel control law is welded into `EvaluateChannel`
  (`src/control/channel_evaluator.cpp:435`) as a feed-forward curve, with its
  dynamic state fused into `ChannelState` (`control_runtime_context.h:17-84`).
  There is no `IChannelController` seam, so the system can swap a law's *numbers*
  but not its *kind*. FEAT-0003 captures what a kind-swappable seam needs and how
  wide it must be (a feedback PID is the worked stress-test example).
- **Idea** — FEAT-0003 is design-capture of the required *range*, not scheduled
  work; the maintainer does not currently believe PID is a net benefit. Revisit
  only for a demonstration.
- **Idea (verify)** — `docs/CONTROL_PIPELINE_MATH.md` §6.1 describes the rising
  boost integrator as gated on `B < B_max`, while
  `src/control/boost_stage.cpp` enforces the cap via a final clamp (behaviorally
  equivalent for the shipped specs). Align the prose to the code or confirm the
  equivalence in a note.
- **Idea** — `docs/BUILD_TARGETS_AND_DEPENDENCIES.md` now also covers
  driver/hardware access, beyond its build-targets title. If that scope is
  unwanted, split the Drivers and Hardware Access section into a dedicated
  `docs/HARDWARE_ACCESS.md` and leave a pointer.

## 2026-05-30

- **Fixed** — scheduled-task status reporting made resilient to CIM/WMI query
  failures (`73ce45a`).
- **Done** — runtime CSV rows are descriptor-driven; header and row builders
  share descriptor tables, with alignment checked by
  `svg_mb_control_csv_rows_tests`; docs compacted (`9572d18`).

## 2026-05-29

- **Done** — native `analyze` made a superset of the prior Python flow;
  `scripts/analyze_control_run.py` reduced to a thin wrapper (`554cba1`).
- **Done** — script stack simplified per the `SCRIPT_STACK_REVIEW` backlog
  (`f358f48`).

## 2026-05-28

A structural-refactor day; behavior held constant (see
`docs/STRUCTURE_AND_STABILITY.md` §Migration Order for the canonical list).

- **Done** — boost overlays converted to a table-driven `UpdateBoostStage`
  (`kBoostStageSpecs`); 21 legacy `ChannelControlConfig` fields and four legacy
  `ChannelState` doubles deleted; CSV/JSON/banner output kept byte-identical,
  locked by `svg_mb_control_boost_stage_tests`
  (`d8ddaad`→`0882342`, `b442bef`, `336c411`).
- **Done** — `EvaluateChannel` split into five single-purpose helpers
  (`cca748a`); `app_main.cpp` split into `app_args` / `app_signals` /
  `app_diagnose` (`3191daf`); `analyze_report.cpp` split with
  report/manifest/decision-record outputs (`53bf8cf`); shared math primitives
  extracted to `control_math` (`9deff86`).
- **Done** — `runtime_write_policy` + write-once orchestrator re-homed to
  `src/runtime/`; `src/policy/` left with curve/blend math only (`d18af9e`).
- **Added** — C++ unit tests for `control_math`, `analyze_report` helpers, and
  boost-stage config (`883942d`).
- **Fixed** — low-band gates named (`CONTROL_SIMPLIFICATION_TARGETS` #3/#4,
  `98da9a9`); `tick_channel_samples` columns centralized in the analyze module
  (`c88379f`); `runtime_csv_archive`'s local `Sha256FileHex` copy dropped in
  favor of the shared `platform/file_hash` helper (`b8f4f03`).

## 2026-05-27

- **Added** — machine cooling policy and intake curves
  (`config/machines/snd-desk.cooling.policy.json`, `1560614`).
- **Fixed** — native watchdog stale-recovery (`db82e54`); transient runtime IO
  failures hardened (`8c1d662`); `TryWriteJsonFileAtomic` error message surfaced
  in three more callers (`81a7061`).

---

## Ideas / backlog (not scheduled)

Each line is a candidate, not authorized work. Promote via `docs/features/`
before building.

- **Control-law seam** — `IChannelController` so the control *kind* (not just its
  tuning) is swappable. Captured in `FEAT-0003`; design-capture only.
- **FEAT-0001 / FEAT-0002 status** — `FEAT-0001` is `Accepted` (gate 3 cleared
  2026-06-06), not yet implemented. `FEAT-0002` shipped its load layer with
  `cpu_settings_label` (`REQ-CPUSETTINGS-06`) deferred. Neither is open
  promotion work now.
- **CONTROL_PIPELINE_MATH §6.1 prose-vs-code** — align or annotate (see
  2026-06-03 entry).
- **Doc scope** — consider `docs/HARDWARE_ACCESS.md` split (see 2026-06-03 entry).
- **Remaining structural polish** (`docs/STRUCTURE_AND_STABILITY.md` §Remaining):
  module-qualified include paths; split Python smoke tests by runtime mode;
  separate build/package from live deploy so local verification never stops the
  controller.
- **VBS/HVCI empirical check** — verify hardware-access behavior with Memory
  Integrity on via `--diagnose-amd` / `--diagnose-gpu` (labeled recommendation
  from `docs/BUILD_TARGETS_AND_DEPENDENCIES.md`).
