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
- **FEAT-0001 / FEAT-0002 promotion** — both are `Draft`; FEAT-0001's decision
  record exists, FEAT-0002 still needs its dated decision record before it is
  buildable (see each spec's gate-3 line).
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
