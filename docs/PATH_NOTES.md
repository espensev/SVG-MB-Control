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

## 2026-06-10

- **Added** — power-anticipation design-support increment (no control wiring;
  the live path stays gated per the feed-forward plan):
  `src/control/power_anticipation.h` (pure watts-derivation + hold/stale
  input + decay-on-unavailable integrator math),
  `tests/cpp/power_anticipation_tests.cpp` (CTest 12/12 now), and the Gate 2
  measurement tool `scripts/analyze_power_lead.py` (+
  `tests/test_power_lead.py`; lag-scanned power→Tctl and busy→Tctl lead,
  idle-floor watts, window coverage — busy legs usable on existing disabled
  CSVs). `BUILD_TARGETS_AND_DEPENDENCIES.md` §Test executables and the plan
  doc §4/§5/§7 updated. Validated: `Test-LocalCI` exit 0 (CTest 12/12,
  hermetic Python lane 133 tests).
- **Added** — `docs/testing-and-hotpath-simplification-review-2026-06-10.md`:
  two-sweep simplification review (test/script stack + runtime hot paths),
  every kept finding re-verified at the cited lines. Net: no critical
  findings; actionable cleanups are TS-1/TS-2 (shared C++ test helper header,
  ~120 LOC), TS-3 (shared Windows-gate test base, ~40 LOC), HP-1 (delete the
  test-only 3-arg `BuildControlLoopCsvRow` overload); one sweep claim
  rejected (Build-Release double hash is deliberate copy verification).
- **Added / Idea** — `docs/cpu-power-feedforward-plan-2026-06-10.md`: planning
  scaffold for a CPU package-power anticipation boost (a fifth
  `kBoostStageSpecs` stage keyed on derived watts), explicitly NOT authorized
  work — the v1 energy decision bans control use, so the path is gated:
  quarantine exit (Gate 0) → default-on decision (Gate 1) → lead-time
  characterization with an explicit no-go and a `system_cpu_busy_pct`
  alternative comparison (Gate 2) → new FEAT intake (Gate 3), then
  shadow-mode → replay → capped live phases.
- **Fixed** — stale deployment claims about the 2026-06-09 GPU retune:
  `gpu-response-curve-retune-2026-06-09.md` §5 said the retuned config was
  "not built, published, or restarted on the live worker", and
  `response-evaluation-tuning-plan.md` framed the retune as "pending ... not
  yet deployed" (including in yesterday's drift-fix pass). Both predate the
  2026-06-09 live publish. Verified read-only: `release\control.json` is
  byte-identical to `config\control.release.json` (sha256 `b51e542a...`), and
  the live worker's CSV session header records the same `config_sha256` with
  exe `git_hash=c4b6986` — the config was published before the retune commit
  `767a901` existed, a provenance-stamp mismatch only.
- **Added** — `docs/cpu-energy-quarantine-exit-capture-runbook-2026-06-10.md`:
  operational runbook for one enabled-path CPU-energy evidence session
  (enable via env var + scheduled-task restart, capture an
  idle/load/cooldown profile, revert to default-off, analyze, score against
  the quarantine-exit Evaluation). Includes the criterion-6 loop-slip
  baseline from the 2026-06-09/10 disabled session (p95 `1.56 ms`,
  ~6.5 overrun rows/h) and a RAPL-independence classification of the
  available external power references for criterion 3. Prepared, not yet
  executed; the criteria stay normative in
  `docs/cpu-work-energy-acquisition-decision-2026-06-07.md`.
- **Done** — resolved GPU-heat review finding F-DES1 (maintainer, option C):
  the channels `1`/`5` `thermal_pressure` boost firing on GPU heat at
  `>= 85.5 C` is accepted and documented as case-pressure headroom rather than
  capped. Rationale and limits are recorded in `docs/COOLING_STRATEGY.md`
  (Radiator Exhaust Pair); the open question is marked resolved in
  `docs/gpu-heat-response-chain-review-2026-06-09.md` and
  `docs/gpu-response-curve-retune-2026-06-09.md`.
  `config/control.release.json` is unchanged, so the CPU response path is
  untouched.

## 2026-06-09

- **Done** — docs cleanup pass: archived three dead point-in-time review
  snapshots under `docs/archive/` (`build-optimization-results`,
  `code-quality-pass-2026-05-19`,
  `evaluation-and-optimization-recommendations`) and consolidated the five
  overlapping Bench-vs-Control logging discovery notes into
  `docs/bench-logging-history.md`. The originals were moved to
  `docs/archive/` for audit history; `AGENTS.md`, `README.md`, `CODE_MAP.md`,
  and `next_steps.md` now point at the consolidated/current shape.
- **Fixed** — docs drift from the 2026-06-09 GPU-response work:
  `source-aware-blend-decision-2026-05-26.md` now marks the channels `1`/`5`
  `cpu_only` statements as true for the 2026-05-26 deployment but superseded by
  the radiator-exhaust GPU response decision; `response-evaluation-tuning-plan.md`
  now separates the deployed `release\control.json` baseline from the pending
  repo-config GPU retune.
- **Done** — collapsed the nine duplicated core-linked CTest registration
  blocks in `CMakeLists.txt` into a `svg_mb_control_add_core_test(name source)`
  helper plus nine one-line calls; the two header-only tests (`rapl_energy` /
  `cpu_cycles`) stay hand-rolled because they must not link
  `svg_mb_control_core`. Behavior-preserving: the same eleven CTest targets,
  names, and registration order, verified by `Test-LocalCI` (CTest 11/11,
  Python hermetic lane green). Adding a future core-linked test is now one
  line. Logged as `docs/STRUCTURE_AND_STABILITY.md` §Migration Order item 16.
- **Fixed** — `docs/BUILD_TARGETS_AND_DEPENDENCIES.md` §Test executables said
  "Each links `svg_mb_control_core`", which was untrue for the two header-only
  tests added the same day; restated to name the two exceptions and the reason
  (`rapl_energy.h` / `cpu_cycles.h` math is dependency-free).

## 2026-06-06

- **Changed** — revised decision **D6** (profile hot-swap live authorization) to the
  **B1+B2** posture (maintainer, 2026-06-06): `pid.allow_live: true` is rejected at
  config load unless the channel has characterization evidence (a shadow-log
  comparison against the curve baseline is accepted) *and* a non-NaN slew cap. This
  reverses, by intent, the align-`REQ-PROFILE-07`-to-D6 direction recorded below —
  the review found `MEASUREMENT_GATE.md` reads as an evidence (not consent) gate and
  the slew-cap floor is NaN-by-default (`control_loop.h:29-31`;
  `channel_evaluator.cpp:55-57`). Propagated source-of-truth-first: D6 →
  `REQ-PROFILE-07` (FEAT-0003 §4/§5/§6/§7/§9/§10/§11/§12; verify now `T,R,M`) →
  `docs/TRACEABILITY.md` → modular plan §6/§7-8; the brief
  `docs/profile-hot-swap-allow-live-decision-2026-06-06.md` is marked **Resolved
  (B1+B2)**. Refreshed the discussion doc's measurement-gate section (its
  evidence-vs-consent argument was *adopted*; added a resolution note and fixed
  drifted decision-doc citations). Citation sweep + line→section de-drift: converted
  doc-to-doc cites in the plan and brief to section refs, and fixed the cites my D6
  expansion drifted (decision §D7 / §Consequences / §Selected directions; FEAT-0003
  §6). `tests/test_feature_specs.py` green (5/5). The discussion doc stays unstaged
  (its other in-flight de-drift edits are the user's).
- **Fixed** — reconciled three cross-doc inconsistencies in the `FEAT-0003`
  profile-hot-swap design-capture (surfaced by review). **High:**
  `REQ-PROFILE-07` carried the pre-decision "characterization evidence before any
  live PID write" lean, contradicting decision `D6`, which authorizes live PID via
  an explicit per-channel `pid.allow_live` opt-in *without* prior evidence. Aligned
  the requirement to `D6` (a requirement derives from its decision —
  `docs/features/README.md` §3 gate 4) across FEAT-0003 §4/§5/§6/§10/§12 and
  `docs/TRACEABILITY.md`, and named the decision record the source of truth for
  this path. That resolves the *requirement-vs-decision divergence* (the
  requirement no longer contradicts `D6`); whether `D6`'s `allow_live` actually
  *satisfies* `docs/MEASUREMENT_GATE.md` is a separate, open maintainer call — the
  discussion doc's measurement-gate section argues the gate is evidence-not-consent
  and that the slew-cap floor is NaN-by-default (`control_loop.h:29-31`;
  `channel_evaluator.cpp:55-57`). Tightening to evidence-first is a change to `D6`,
  not the requirement. **Medium:** FEAT-0003 §7 proposed a `runtime/requests/profile.json`
  subdir while claiming to model the breaker-reset file; corrected to the flat-root
  `profile.request.json`, the only implemented convention
  (`circuit_breaker_reset.request.json` / `stop.request.json`,
  `src/runtime/runtime_lifecycle.cpp:17,22`). **Low:** FEAT-0003 §9 decision-row
  status said "Proposed — awaits maintainer acceptance"; updated to the selected
  `D2`/`D3a`/`D6` directions, consistent with §11/§13 and the decision doc's
  "Selected directions." Reframed
  `docs/modular-profile-hotswap-plan-2026-06-06.md` §6/§7-4/§7-8: the
  requirement-vs-decision divergence is resolved, the substantive D6-vs-gate
  question is flagged open, and FEAT-0001's matching `runtime/requests/` subdir is
  corrected the same way (flat-root `write_policy.request.json`). Corrected the
  drifted FEAT-0003 line citation in the plan (`:192`→`:198`)
  and converted the discussion doc's drifted citation to a section ref
  (`:197-202`→`§7`). `tests/test_feature_specs.py` green (5/5). D6's *decision* is
  unchanged (only a dated reconsideration pointer appended to the decision doc);
  the discussion doc's other in-flight edits are the user's, left untouched.
- **Added** — `docs/profile-hot-swap-allow-live-decision-2026-06-06.md`: a
  reconsideration brief for D6's `pid.allow_live` posture (Status: Open), drafting
  **accept-as-is vs tighten** after the discussion doc's measurement-gate section
  argued `MEASUREMENT_GATE.md` reads as an evidence gate, not a consent gate.
  Recommends a non-NaN slew-cap parse precondition (B2) unconditionally +
  shadow-default; B1 (evidence-before-live, which re-tightens REQ-PROFILE-07) is the
  maintainer's read of the gate. Linked from D6 (a "Reconsideration (2026-06-06)"
  note) and plan §7-8. **Checked:** the slew cap is NaN-by-default in code
  (`control_loop.h:29-31`; `channel_evaluator.cpp:55-57` returns unmodified on NaN),
  but shipped `config/control.release.json` sets it for all live channels 0–5 — a
  latent precondition gap, not an active live defect.
- **Changed** — acted on a multi-agent review of the feature-governance batch
  (verdict: the system is sound and CI-enforced; the excess is concentrated in
  speculative specs, not the machinery). Demoted `FEAT-0005`/`FEAT-0006` from
  full Draft specs to **Reserved**: bodies parked under
  `docs/features/_parked/`, registry rows delinked, and their
  `REQ-ACTCONFIRM-*` / `REQ-CPUEFF-*` rows dropped from `docs/TRACEABILITY.md`.
  `FEAT-0004` kept at Draft as the one keeper (a present, operator-visible
  PawnIO-absent gap). Two doc-fills: `TRACEABILITY.md` §1 now states the
  enforcing test checks structure, not that cited checks exist/pass;
  `docs/features/README.md` §2 names the "Draft with all gates checked, held as
  design-capture" state (`FEAT-0003`). `tests/test_feature_specs.py` stays green
  (5/5) after the demotion.
- **Added** — `docs/modular-profile-hotswap-discussion-2026-06-06.md`: a
  non-normative design-commentary companion to the plan doc — a grounded,
  two-sided discussion of each element (premise, skeptic's case, seam, state
  decouple, conditioning split, PID-as-example, measurement gate, Proposals A/B,
  reuse machinery, dependency order, the 8 questions), each closing with the one
  fact that would flip its judgment. Net position: the seam is worth *designing*
  and step-1 conditioning-extraction has standalone merit, but not worth
  *building* under FEAT-0003's current status. Authorizes nothing. Adversarially
  reviewed (5 agents) for doctrine + ref accuracy; cross-doc citations to the
  plan are by section (not line) to resist drift.
- **Added** — `docs/modular-profile-hotswap-plan-2026-06-06.md`: deepens
  `FEAT-0003` design-capture on the "change the entire per-channel control
  function at runtime" idea — the *modular* angle the existing D1–D7 decisions
  left open. Adds two labeled proposals (A: law-dispatch shape — hard-named
  switch vs registry table, recommend "A1 now, reversible to A2"; B: what a
  "profile" object is + a `--profile` startup surface, recommend named on-disk
  profiles), a grounded "what we have today" inventory of `EvaluateChannel`, the
  implemented-vs-spec hot-swap machinery, a design-capture dependency order
  (seam-extraction refactor first, bit-identity gated), and 8 open maintainer
  questions. Status guard: FEAT-0003 stays design-capture, not scheduled, not a
  net benefit — this authorizes nothing.
- **Fixed** — accuracy-only file:line drift in the two 2026-06-03 profile-hotswap
  docs: recovery-gap remediation 3 (`33f8d24`/`68a6fc4`) added
  `ChannelEvaluation::safety_override`, shifting `channel_evaluator.{cpp,h}` refs
  ~5 lines. Corrected `EvaluateChannel` `435-469`→`440-474`, `EvaluatePrimarySetpoint`
  `243-278`→`243-283`, the boost-sum `348-359`→`353-364`, `ChannelEvaluation`
  `h:30-41`→`h:30-47`, and sensor-safe `248-259`→`248-264` in
  `FEAT-0003-selectable-profile-hot-swap.md` and
  `profile-hot-swap-decision-2026-06-03.md`. No decision changed; correction table
  in the new plan doc §8.
- **Added** — feature-intake guardrail for specs-before-build:
  `AGENTS.md` now requires new capabilities, runtime schema/status/log/manifest
  fields, CLI/operator surfaces, live-runtime workflows, and shipped config
  behavior to map to an implementation-authorized `docs/features/FEAT-*` spec
  before product code starts. Added `docs/TRACEABILITY.md` as the central
  `REQ-*` to verification map, and updated the feature template/README so new or
  changed requirements must update that map in the same change. Follow-up:
  `tests/test_feature_specs.py` now checks feature registry/status, `REQ-*`
  coverage in spec sections 6/10/14, traceability rows, accepted/implemented
  promotion-gate state, and implemented verification-log results in the Python
  lane; `docs/FEATURE_VERIFICATION_CHECKLIST.md` records the human handoff
  checklist.
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
- **RAM temperature telemetry** — surface per-DIMM temps (`Agent0/1 DIMM0/1`
  Nuvoton sources) into the read/control snapshot, CSV, status, and analyze.
  Read path already exists: `evidence-log` reads all 23 SVG-MB-SIO temperature
  sources (`read_sio_temperatures`); the gap is promoting the DIMM-labeled ones
  into the normal telemetry surfaces. Captured in `FEAT-0007` (parked; candidate
  for promotion only after the open gates clear); open item is confirming the
  DIMM sources read `valid` on snd-desk from existing `evidence-log` output.
