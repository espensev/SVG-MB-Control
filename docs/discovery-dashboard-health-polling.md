# Discovery - Dashboard Health Polling

**Status:** compacted historical record, 2026-06-20.

This discovery note is closed. It was useful while the dashboard and runtime
health surfaces were being discovered, but the maintained contracts now live
elsewhere.

Use these current records instead:

- `docs/RUNTIME_HOME.md` for `control_runtime.json`, `current_state.json`,
  `logging_health.json`, manifests, latest mirrors, and archive layout.
- `docs/RUNTIME_LOGGING_AND_EVALUATION.md` for analyzer and evaluation workflow.
- `docs/SERVICE_PROBE.md` for the service feasibility probe contract.
- `tools/eval_dashboard/` and its tests for the local dashboard implementation.
- `docs/features/FEAT-0022-runtime-logging-failure-visibility.md` for logging
  health, status/snapshot retry, and mirror/archive/manifest consistency
  behavior.

Closed conclusion preserved from the original:

- Dashboard polling must remain observational. It reads runtime sidecars and
  archived evidence; it must not become a fan-write or runtime-control surface.
