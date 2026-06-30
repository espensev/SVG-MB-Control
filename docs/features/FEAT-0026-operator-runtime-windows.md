# FEAT-0026: Operator runtime windows

**Project:** svg-mb-control
**Status:** Implemented (operator helper + dry-run verification; live execution not exercised)   **Version:** 0.1   **Updated:** 2026-06-30
**Namespace:** `REQ-OPWINDOW-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/CONTROL_LOOP.md`, `docs/RUNTIME_HOME.md`,
`docs/RUNTIME_LOGGING_AND_EVALUATION.md`,
`docs/OPERATOR_RUNTIME_WINDOWS.md`,
`docs/operator-runtime-window-decision-2026-06-30.md`
**Purpose:** provide a packaged operator workflow for bounded stop/pause windows,
manual resume/restart, optional read-only logging while Control is off, and a
machine-readable status surface for future external coordination.

## 1. Summary

This feature adds `Set-SVG-MB-ControlRuntimeWindow.ps1`, a release-packaged
PowerShell helper for intentional runtime windows. It wraps the existing
packaged lifecycle commands and scheduled-task surfaces so an operator can stop,
restart, pause for a duration, resume, and optionally keep read-only evidence
logging during a bounded off window.

The helper also exposes `-Status -Json` as the future coordination surface for
external tools such as SQ-control. That keeps coordination at a process
boundary and avoids a sibling-repo runtime dependency.

## 2. Problem & motivation  *(promotion gate 1)*

The shipped operator surface already has `--stop`, `--restart`, the main
scheduled task, and the watchdog task, but a bounded off window previously
required a manual sequence: disable the main task, stop Control, register a
one-shot resume task, and keep the watchdog from undoing the stop. Memory from
the 2026-06-28 off-window run records that pattern as correct but manual:
disable `SVG-MB Control`, stop cooperatively, then resume through a one-shot
task.

The other gap is logging during intentional control-off windows. Control-loop
CSV logging stops when Control stops. The repo already has foreground
`evidence-log`, which is read-only and writes separate evidence artifacts, but
there was no packaged helper tying it to a bounded off window and stopping it
before Control resumes.

## 3. Goals & non-goals

**Goals**

- Provide one documented helper for `-Status`, `-Restart`, `-Stop`, bounded
  `-Pause`, and `-Resume`.
- Support durations such as `30m`, `1h`, `2h30m`, and exact `-Until` times.
- Disable the watchdog and main task before stopping Control for an off window.
- Register a one-shot resume task for bounded windows.
- Optionally start read-only `--mode evidence-log` during bounded windows and
  stop it before Control resumes.
- Keep a `-DryRun` mode that does not query or mutate live Task Scheduler state.
- Expose machine-readable `-Status -Json` output for future coordinators without
  requiring them to parse human status text.

**Non-goals**

- No new in-process pause state or runtime daemon.
- No direct fan-duty write, no breaker reset, and no control-computation change.
- No replacement for normal control-loop CSV logging while Control is running.
- No sibling-repo, subprocess bridge, or external logging process dependency.
- No direct SQ-control dependency. Future SQ-control coordination should invoke
  this packaged helper or the packaged exe as a process boundary.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| Repo stays standalone | `AGENTS.md` §Repo Boundary | The helper is in this repo, uses the packaged exe, and reuses in-repo scheduled-task helpers. |
| Live Runtime Safety | `AGENTS.md` §Live Runtime Safety | The helper only acts when explicitly invoked, uses existing `--stop`/`--start`/`--restart`, and never writes fan duty or resets breakers. |
| Measurement Gate baseline | `docs/MEASUREMENT_GATE.md` | No cadence, channel, curve, write-cooldown, or control strategy changes. |
| Control-computation identity | `docs/CONTROL_PIPELINE_MATH.md` | No control math or status/CSV control identity change. |
| Runtime schema stability | `docs/RUNTIME_HOME.md` | Helper-owned `runtime\operator_windows\*.json` state is additive and not Control-owned schema. Existing runtime files remain valid. |

## 5. Behavior specification

Implemented behavior.

- `Set-SVG-MB-ControlRuntimeWindow.ps1 -Status` prints the main task, watchdog
  task, resume task, evidence-log task, active helper state, and packaged
  `--status` output.
- `-Status -Json` emits JSON with the resolved script/exe/config/runtime paths,
  helper-owned active-window state, task names, known task install/enabled
  state, and an explicit `process-boundary` coordinator contract. It is intended
  for future external coordinators such as SQ-control and avoids human-text
  scraping.
- `-Restart` invokes the packaged `svg-mb-control.exe --restart --config
  <control.json>` path.
- `-Stop` disables and stops the watchdog task, disables and stops the main task,
  then requests a cooperative controller stop through `--stop --config
  <control.json>`. Without `-For` or `-Until`, the controller remains stopped
  until `-Resume`.
- `-Pause -For <duration>` or `-Pause -Until <datetime>` performs the same
  off-window stop and registers a one-shot resume scheduled task. `-For` accepts
  compact duration strings (`30s`, `15m`, `1h`, `2h30m`, `1d`) and TimeSpan
  strings.
- The helper writes `runtime\operator_windows\active_window.json` with the
  resolved exe/config/runtime paths, task names, previous task-enabled states,
  resume time, and optional evidence-log flag. On successful resume it writes
  `last_window.json` and removes `active_window.json`.
- `-EvidenceLog` is valid only for bounded windows. It registers and starts a
  one-shot scheduled task running `svg-mb-control.exe --mode evidence-log
  --config <control.json>`. The task has an execution-time limit longer than the
  off window to bound orphaned logging if resume fails.
- `-Resume` reads the helper state when present, requests evidence-log stop if
  it was enabled, waits for matching evidence-log processes to exit, force-stops
  only remaining matching evidence-log processes after a timeout, then starts
  Control and restores the previous watchdog/main task enabled states.
- `-DryRun` prints planned state writes, scheduled-task changes, lifecycle
  commands, resume-task registration, and evidence-log command lines without
  writing files, querying Task Scheduler, launching processes, or changing
  runtime state.

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-OPWINDOW-01 | The repo must ship one operator helper that exposes status, restart, stop, bounded pause, and resume workflows using the packaged `svg-mb-control.exe` and the repo-defined scheduled-task names. |
| REQ-OPWINDOW-02 | A bounded pause/off window must disable the watchdog and main scheduled tasks before requesting cooperative stop, persist helper-owned state, and register a one-shot resume task for the requested resume time. |
| REQ-OPWINDOW-03 | Resume must stop optional evidence-log before starting Control, then start Control through the documented task or packaged `--start` path and restore previous task-enabled state when known. |
| REQ-OPWINDOW-04 | Optional logging during a bounded off window must use the existing read-only `--mode evidence-log` surface, write separate evidence artifacts, and remain bounded by task execution time and resume cleanup. |
| REQ-OPWINDOW-05 | Dry-run mode must be testable without a live release build or Task Scheduler mutation and must print the lifecycle, task, resume, and evidence-log actions it would take. |
| REQ-OPWINDOW-06 | The helper must be included in release packaging and documented in operator/runtime docs, with no fan-duty write, breaker reset, control-math, runtime-schema, or sibling-repo dependency change. |
| REQ-OPWINDOW-07 | The helper must expose a machine-readable status surface for future external coordinators, including SQ-control, while preserving SVG-MB-Control as a standalone repo with a process-boundary contract. |

## 7. Data / schema deltas

No Control-owned runtime schema changes.

New helper-owned artifacts:

- `runtime\operator_windows\active_window.json`
- `runtime\operator_windows\last_window.json`

These files are written by `Set-SVG-MB-ControlRuntimeWindow.ps1`, not by the
runtime. They record the helper's resume and task-restore state. Existing
runtime-home files, CSV archives, manifests, and status schemas remain valid.

Config impact: none.

Schema/version impact: none.

## 8. CLI / config / operator surface deltas

New release-packaged helper:

```powershell
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Status
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Status -Json
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -For 1h
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -For 45m -EvidenceLog
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Stop
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Resume
.\Set-SVG-MB-ControlRuntimeWindow.ps1 -Restart
```

The helper supports `-DryRun`, `-NoElevate`, `-ExePath`, `-ConfigPath`, and
`-StatePath` for tests/review and non-default package locations. `-Status -Json`
is the supported machine-readable status shape for future coordinators.

## 9. Design decision record(s)  *(promotion gate 3)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| [`docs/operator-runtime-window-decision-2026-06-30.md`](../operator-runtime-window-decision-2026-06-30.md) | Use a repo-local PowerShell helper plus one-shot scheduled tasks and optional evidence-log, rather than a new runtime daemon or in-process pause state. | Current |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-OPWINDOW-01 | T, R | `tests/test_runtime_window_script.py` dry-run restart/status command shape; review helper uses packaged exe and common task names. |
| REQ-OPWINDOW-02 | T, R | Dry-run pause test asserts watchdog/main disable, stop request, state write, and resume-task registration. |
| REQ-OPWINDOW-03 | T, R | Script review of `Resume-ControlFromState` / `Stop-EvidenceLogIfNeeded`; dry-run resume path remains non-mutating. |
| REQ-OPWINDOW-04 | T, R | Dry-run pause with `-EvidenceLog` asserts `--mode evidence-log --config`; review task limit and resume cleanup. |
| REQ-OPWINDOW-05 | T | `tests/test_runtime_window_script.py` runs `-DryRun` with nonexistent explicit exe/config paths and no Task Scheduler mutation. |
| REQ-OPWINDOW-06 | T, R | Packaging list includes `Set-SVG-MB-ControlRuntimeWindow.ps1`; README, `CONTROL_LOOP.md`, `RUNTIME_HOME.md`, logging docs, and `OPERATOR_RUNTIME_WINDOWS.md` updated. |
| REQ-OPWINDOW-07 | T, R | Dry-run `-Status -Json` test parses the output as JSON and asserts `process-boundary` / no sibling dependency; `OPERATOR_RUNTIME_WINDOWS.md` records the SQ-control coordination boundary. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` Live Runtime Safety).
- **R** = code review against the cited contract doc, decision record, or source.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Whether to add a native finite-duration evidence-log mode | Future product-code work only if graceful evidence-log shutdown from scheduled tasks proves insufficient | Keep duration/window orchestration in the PowerShell helper; no runtime mode change. |
| Whether to make helper state Control-owned status | Future dashboard integration | Keep it helper-owned under `runtime\operator_windows`, not part of `control_runtime.json`. |
| SQ-control coordination semantics | Future SQ-control work | Keep this repo standalone; expose `-Status -Json` and packaged lifecycle commands as the process-boundary contract. |

## 12. Measurement gate & dependencies

- **Measurement gate:** not crossed. The feature changes only operator
  lifecycle orchestration and optional read-only evidence logging during off
  windows.
- **Depends on:** existing `--stop`, `--start`, `--restart`, scheduled-task
  install names, and `--mode evidence-log`.
- **Build/test impact:** adds a PowerShell helper, Python dry-run tests, release
  packaging entry, and docs/spec/traceability updates. No C++ build target or
  `CONTROL_PIPELINE_MATH.md` update.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [x] 3. Required design decision record(s) written and marked current (§9).
- [x] 4. Concrete `REQ-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks - `Test-LocalCI`, build-release, contract review, or runtime evidence (§10), and mirrored in `docs/TRACEABILITY.md`.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline.
- [x] 7. Doctrine check: claims are grounded; `must`/`should`/`is` used per `CLAUDE.md`; no undefined terms or unqualified vague adjectives.

## 14. Verification log  *(fill in after the feature is built - "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-OPWINDOW-01 | pass | `tests/test_runtime_window_script.py` covers restart dry-run command shape; script review confirms packaged exe/common scheduled-task names. | 2026-06-30 |
| REQ-OPWINDOW-02 | pass | `tests/test_runtime_window_script.py::test_pause_dry_run_disables_tasks_and_schedules_resume_with_evidence` asserts task disable, stop request, state write, and resume task. | 2026-06-30 |
| REQ-OPWINDOW-03 | pass | Review of `Resume-ControlFromState` and `Stop-EvidenceLogIfNeeded`: evidence-log stop is requested and waited before Control start; prior task-enabled state is restored when known. | 2026-06-30 |
| REQ-OPWINDOW-04 | pass | Dry-run evidence-log test asserts `--mode evidence-log --config`; review confirms separate evidence task, execution-time limit, and resume cleanup. | 2026-06-30 |
| REQ-OPWINDOW-05 | pass | `tests/test_runtime_window_script.py` passes using `-DryRun`, `-NoElevate`, and nonexistent explicit exe/config paths. | 2026-06-30 |
| REQ-OPWINDOW-06 | pass | `scripts/Build-Release.ps1` packages the helper; README, `CONTROL_LOOP.md`, `RUNTIME_HOME.md`, `RUNTIME_LOGGING_AND_EVALUATION.md`, and `OPERATOR_RUNTIME_WINDOWS.md` document the workflow. | 2026-06-30 |
| REQ-OPWINDOW-07 | pass | `tests/test_runtime_window_script.py::test_status_json_dry_run_is_machine_readable_for_coordinators` parses `-Status -Json`; `OPERATOR_RUNTIME_WINDOWS.md` records the process-boundary/no-sibling-dependency contract. | 2026-06-30 |

**Spec vs. implementation deltas:** None for v1. Live scheduled-task execution
has not been exercised in this change; verification is dry-run and review only.
