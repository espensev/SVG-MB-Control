# Discovery - Next Logging Targets

**Date:** 2026-05-16
**Status:** compacted historical record, 2026-06-20.

This discovery note is closed. Its recommendations were either implemented or
superseded by later feature specs and runtime contracts.

Use these current records instead:

- `docs/logging-next-targets-2026-06-18.md` for the promoted logging target
  decision record.
- `docs/RUNTIME_LOGGING_AND_EVALUATION.md` for the maintained analyzer and
  runtime-evidence workflow.
- `docs/RUNTIME_HOME.md` for runtime artifacts, manifests, mirrors, status, and
  health sidecars.
- `docs/features/FEAT-0020-standard-control-loop-power-logging.md` for standard
  control-loop CPU/GPU power logging.
- `docs/features/FEAT-0021-standard-control-loop-gpu-workload-context-logging.md`
  for cached GPU workload context logging.
- `docs/features/FEAT-0022-runtime-logging-failure-visibility.md` for CSV,
  manifest, mirror, event-log, status, snapshot, and analyzer consistency
  visibility.

Closed conclusions preserved from the original:

- Control owns its runtime logging and analyzer path directly; do not reintroduce
  a sibling Bench bridge.
- Live mirrors are mutable and can race readers. Evidence comparisons should use
  pinned closed archives unless explicitly investigating live mirror behavior.
