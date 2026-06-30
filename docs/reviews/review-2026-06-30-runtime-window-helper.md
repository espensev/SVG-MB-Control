# Review - Runtime Window Helper

**Date:** 2026-06-30
**Surface:** Working tree, scoped to FEAT-0026 runtime-window helper files
**Spec source:** `docs/features/FEAT-0026-operator-runtime-windows.md`
**Standards sources:** `AGENTS.md`, `README.md`, `docs/CONTROL_LOOP.md`, `docs/RUNTIME_HOME.md`
**Independent reviewer:** subagent `019f19da-b996-7ab2-b419-87ec2fff4d26`
**Verdict:** PASS WITH NOTES

## Findings

No blocking findings.

### Low

- [axis: verification] Live scheduled-task mutation was not exercised.
  Evidence: `docs/features/FEAT-0026-operator-runtime-windows.md` records the
  feature as "live execution not exercised"; the main and independent review
  used dry-run, packaging, and read-only live status checks.
  Impact: command shape, packaging, JSON status, and current live status are
  verified, but a real timed pause should still get one supervised short-window
  run before unattended use.
  Recommendation: follow `docs/OPERATOR_RUNTIME_WINDOWS.md` "First Live Use"
  before depending on unattended timed windows.

## Verification

- `python -m unittest tests.test_runtime_window_script -v` - pass, 4 tests.
- `python -m unittest tests.test_feature_specs -v` - pass, 5 tests.
- `powershell -NoProfile -ExecutionPolicy Bypass -File .\Set-SVG-MB-ControlRuntimeWindow.ps1 -Pause -For 2h30m -EvidenceLog -DryRun -NoElevate -ExePath D:\tmp\svg-mb-control.exe -ConfigPath D:\tmp\control.json -StatePath D:\tmp\active_window.json` - pass; printed state write, watchdog/main disable, cooperative stop, evidence-log task, and one-shot resume task.
- `powershell -NoProfile -ExecutionPolicy Bypass -File .\Set-SVG-MB-ControlRuntimeWindow.ps1 -Status -Json -DryRun -NoElevate ... | ConvertFrom-Json` - pass; emitted schema version 1, `coordinator_contract=process-boundary`, and `sibling_repo_dependency=false`.
- `D:\Development\Thermals\SVG-MB\SVG-MB-Control\build-release.ps1 -NoStopProcesses -NoPublish -SkipTests -KeepBuildDir` - pass; packaging copied `Set-SVG-MB-ControlRuntimeWindow.ps1` and `docs`.
- `git diff --check` - pass; Git reports the existing LF-to-CRLF warning for `scripts/Build-Release.ps1`.
- `powershell -NoProfile -ExecutionPolicy Bypass -File .\Set-SVG-MB-ControlRuntimeWindow.ps1 -Status -Json -ExePath .\release\svg-mb-control.exe -ConfigPath .\release\control.json` - pass; read-only live status resolved packaged paths, found the main task installed/enabled, and found no active operator window.
- `.\release\svg-mb-control.exe --status --config .\release\control.json` - pass; runtime reported `running`, `mode: control-loop`, `hwaccess: available`, active supervisor/worker, and `worker_restart_count: 0`.
- `.\release\svg-mb-control.exe --health --json --config .\release\control.json` - pass; runtime reported `health_state=healthy`, `process_active=true`, `supervisor_active=true`, and `stop_request_present=false`.
- `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` - fail outside this feature's scope: CTest `svg_mb_control_profile_composition_tests` failed against staged `config/control.release.json` profile values. The failure matches the pre-existing staged config delta and not the runtime-window helper.

## Coverage Notes

- Deep-reviewed files: `Set-SVG-MB-ControlRuntimeWindow.ps1`, `tests/test_runtime_window_script.py`, `docs/features/FEAT-0026-operator-runtime-windows.md`, `docs/operator-runtime-window-decision-2026-06-30.md`, `docs/OPERATOR_RUNTIME_WINDOWS.md`, `README.md`, `docs/CONTROL_LOOP.md`, `docs/RUNTIME_HOME.md`, `docs/RUNTIME_LOGGING_AND_EVALUATION.md`, `docs/BUILD_TARGETS_AND_DEPENDENCIES.md`, `docs/TRACEABILITY.md`, `docs/features/README.md`, `scripts/Build-Release.ps1`.
- Excluded from the verdict: pre-existing or unrelated changes in `config/control.release.json`, `src/analyze/*`, FEAT-0023 docs, `data/`, observer docs, and other review/discovery artifacts.
- Independent subagent review found no blocking issues and independently ran the runtime-window and feature-spec test lanes.

## Open Questions

- When the operator is ready for live mutation, run one supervised short timed window to validate Task Scheduler registration/resume on the installed machine.
