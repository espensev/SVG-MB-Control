# SVG-MB-Control Documentation

This is the local documentation map for `SVG-MB-Control`. The GitHub repository
landing page is the root [README.md](../README.md); there is no separate
GitHub Pages site for this repo.

## Start Here

- [README.md](../README.md) — product scope, default build/run/analyze/config
  workflows, runtime-home summary, tests, and the GitHub-facing overview.
- [STRUCTURE_AND_STABILITY.md](STRUCTURE_AND_STABILITY.md) — current source
  layout, responsibility boundaries, and remaining structural polish.
- [BUILD_TARGETS_AND_DEPENDENCIES.md](BUILD_TARGETS_AND_DEPENDENCIES.md) —
  executable targets, scheduled-task processes, helper tools, vendored
  dependencies, and hardware-access model.
- [CODE_MAP.md](CODE_MAP.md) — per-file responsibility map for quick
  navigation.

## Runtime Contracts

- [CONTROL_LOOP.md](CONTROL_LOOP.md) — supervised control-loop lifecycle,
  status, scheduled-task behavior, and helper integration.
- [READ_LOOP.md](READ_LOOP.md) — direct read-loop behavior and status/logging
  contract.
- [WRITE_ORCHESTRATION.md](WRITE_ORCHESTRATION.md) — write policy, write-once,
  pending-write sidecars, restore, and breaker behavior.
- [RUNTIME_HOME.md](RUNTIME_HOME.md) — runtime sidecars, status fields, health
  behavior, logs, manifests, and archive retention.
- [RUNTIME_LOGGING_AND_EVALUATION.md](RUNTIME_LOGGING_AND_EVALUATION.md) —
  runtime evidence collection, analyzer workflow, and logging gaps.
- [OPERATOR_RUNTIME_WINDOWS.md](OPERATOR_RUNTIME_WINDOWS.md) — intentional stop,
  pause, resume, restart, and read-only evidence-log windows.

## Control Policy And Tuning

- [CONTROL_PIPELINE_MATH.md](CONTROL_PIPELINE_MATH.md) — maintained numerical
  reference for curve lookup, smoothing, boosts, low-band behavior, cadence, and
  CSV/status identities.
- [CONTROL_PID_MATH.md](CONTROL_PID_MATH.md) — PID math reference for the
  selectable controller profile work.
- [COOLING_STRATEGY.md](COOLING_STRATEGY.md) — strategy, fan inventory, floor
  philosophy, and shipped fan-relationship rules.
- [NORMAL_RUNTIME_AIRFLOW_PROFILE.md](NORMAL_RUNTIME_AIRFLOW_PROFILE.md) —
  normal runtime airflow profile notes.
- [MEASUREMENT_GATE.md](MEASUREMENT_GATE.md) — evidence requirements before
  moving cadence, channel, live-write, or control-strategy baselines.
- [response-evaluation-tuning-plan.md](response-evaluation-tuning-plan.md) —
  current controller tuning workflow.

## Feature Intake And Verification

- [features/README.md](features/README.md) — current priority index, decisions
  owed, feature registry, and spec lifecycle.
- [TRACEABILITY.md](TRACEABILITY.md) — `REQ-*` to verification mapping.
- [FEATURE_VERIFICATION_CHECKLIST.md](FEATURE_VERIFICATION_CHECKLIST.md) —
  implementation and handoff checklist for feature work.

Before product-code work starts for a new capability, behavior change, runtime
schema/status/log/manifest field, CLI/operator surface, live-runtime workflow,
or shipped config behavior, identify the owning accepted and
implementation-authorized `docs/features/FEAT-*` spec.

## Journals And Historical Material

- [PATH_NOTES.md](PATH_NOTES.md) — curated dated journal of completed/fixed/added
  work and ideas. It is not a system of record.
- `docs/archive/` — point-in-time plans, discoveries, recommendations, and
  completed implementation records.
- `docs/reviews/` — review artifacts.
- `docs/discovery-*.md` — discovery snapshots. Treat these as historical unless
  a current authority doc links them as active context.

Authoritative current claims should be verifiable from source, tests, runtime
evidence, config, `git log`, or the current contract docs linked above.
