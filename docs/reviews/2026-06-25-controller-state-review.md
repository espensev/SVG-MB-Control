# Controller State Review - 2026-06-25

## Scope

Read-only controller state review from the current checkout and the live
`release\runtime` sidecars. No controller start, stop, restart, scheduled-task
change, breaker reset, or fan-duty write was performed.

## Verdict

The live controller is healthy right now, but it is not running the current
source/config state.

The active runtime is the ignored `release\` package built from commit
`ba83aed14a58` with config hash
`45a0a1c732a04f9aa3933d059b7a095b24d03b5034f0fbf541b717eb2f7557af`.
Current `HEAD` is later (`f445fc1` at review time), and
`config\control.release.json` now contains FEAT-0024 intake-lead retune values
that are absent from the live `release\control.json`.

Do not interpret current live behavior as evidence for FEAT-0024 or FEAT-0025.

## Live State

Evidence from `release\runtime\control_runtime.json`,
`release\runtime\control_health.json`,
`release\runtime\control_supervisor.json`, and
`release\runtime\logs\svg_mb_control_manifest.json`:

- `status=running`, `mode=control-loop`, `process_id=8508`.
- Health is `healthy`; reason: `runtime process is active and status is fresh`.
- Hardware access is available on both paths:
  `hwaccess_read_state=available`, `hwaccess_write_state=available`.
- Timing is currently nominal: `poll_tick_ms=250`, latest achieved interval
  about `251.25 ms`, `loop_overrun=false`, `loop_work_duration_ms` about
  `1.55 ms`.
- All controlled channels report `controller_kind=curve_overlay`; no live PID or
  alternate law is active.
- Channels `0-5` report no circuit breakers, no sensor failures, no write
  failures, and no sidecar persist failures.
- Active profile is `control` from `explicit_config`.

## Findings

### 1. Live package is stale relative to current source

`release\build-info.json` reports:

- `builtUtc=2026-06-23T06:24:56Z`
- `sourceCommit=ba83aed14a584ae76a0c1da4ed7e00b03c03c1ff`
- `workingTreeDirty=false`

The active runtime manifest reports the same producer hash (`ba83aed14a58`) and
the live config path `release\control.json`.

`git diff --no-index -- config\control.release.json release\control.json`
shows the current source config has FEAT-0024 intake changes not present in the
live package:

- channel `4`: rise rate `120` in source vs `60` live package, step cap `0.95`
  vs `0.6`, `gpu_airflow_start_c=58` vs `64`, `gpu_airflow_max_boost_pct=10`
  vs `5`, and steeper `cpu_override` points at `82 C` / `86 C`.
- channels `2` and `3`: rise rate `125` in source vs `90` live package, step
  cap `0.95` vs `0.7`, `gpu_airflow_start_c=58` vs `62`, and
  `gpu_airflow_max_boost_pct=12` vs `8`.

Impact: the running controller is using the older intake response. Any current
runtime observations are for the `ba83aed` package, not current `HEAD`.

Recommended next step: if FEAT-0024 live adoption is intended, run the repo
release workflow and the owed Pass-1/Pass-3 evidence under explicit live-runtime
authorization. If adoption is not intended, keep calling the source values
"candidates" and avoid claiming they are live.

### 2. FEAT-0024 gate state and source config are out of phase

`docs\TRACEABILITY.md` and `docs\features\FEAT-0024-intake-lead-under-load.md`
still mark FEAT-0024 as Draft/held and not buildable, with `REQ-INLEAD-*`
verification pending. At the same time, `config\control.release.json` already
contains the retune values and `tests\test_config_contracts.py` asserts those
values as the source-of-truth release config.

Impact: the spec-before-build boundary is ambiguous. The code/config side looks
implemented in source, while the feature-governance side still says held Draft
with live validation owed.

Recommended next step: choose one state explicitly:

- keep FEAT-0024 as a source-side candidate and avoid publishing it until the
  Pass-1/Pass-3 gate closes; or
- promote/record the implementation state and fill the verification log after
  live evidence exists.

### 3. Recent watchdog recovery occurred

The active event log records a recent worker exit and recovery:

- `2026-06-25T17:27:17`: `supervisor.worker_exited`, `exit_code=1`.
- `2026-06-25T17:27:20`: new worker started.
- `2026-06-25T17:27:20`: `supervisor.worker_force_terminated` for the prior
  hung worker.

The controller is healthy after the restart, but this is still a real recent
recovery event. Startup authority reassertions after each worker start appear
bounded to tick `1`, which is expected for restart ownership recovery.

`release\runtime\svg-mb-control.worker.stderr.log` and supervisor stderr also
contain repeated historical messages:

- `restore-auto succeeded but sidecar flush failed: Failed to replace JSON output file: Windows error 5`
- `pending writes reconciliation failed. Refusing to proceed.`
- repeated `failed to write runtime log manifest` warnings

The current health sidecar does not report active logging failure
(`logging_health_present=false`), and the manifest is updating now. Treat these
stderr messages as recent-history evidence to correlate, not as proof of an
active failure.

Recommended next step: if the 17:27 recovery was unexpected, correlate that
timestamp with scheduled task runs, build/publish activity, and file locks on
`release\runtime`. If repeated, add a small defect item around reconcile/manifest
write contention visibility.

### 4. FEAT-0025 is correctly not active

Current `src\control\channel_evaluator.cpp::RateLimitSetpoint` still sizes the
rate budget from the uncapped `elapsed_ms`. The FEAT-0025
`rate_limit_max_elapsed_ms` field is present only in Draft docs and traceability;
no config or source implementation is active.

Impact: if the operator symptom is still "less tight under load," current live
behavior can still exhibit the elapsed-driven slipped-tick overshoot described
by FEAT-0025. That is not a regression in this review; it is pending,
not-buildable work.

## Validation Run

- `python -m unittest tests.test_config_contracts tests.test_feature_specs`
  passed: `22` tests.
- `git status --short` showed only untracked `.superpowers/`.
- Process command-line inspection and direct `release\svg-mb-control.exe
  --status/--health` launches were blocked by the sandbox
  (`CreateProcessAsUserW failed: 5`), so this review relies on runtime sidecars,
  manifest, ignored release package metadata, and event/log file search.
- Full local CI was not run because this was a read-only state review and no
  product code was changed.
