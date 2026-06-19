# Discovery - GPU Response Refinement

**Status:** compacted historical record, 2026-06-20.

This discovery note is closed. It predates the maintained cooling-policy docs,
standard control-loop power logging, and GPU workload-context logging. Do not use
it as a tuning source without fresh runtime evidence.

Use these current records instead:

- `docs/COOLING_STRATEGY.md` and
  `config/machines/snd-desk.cooling.policy.json` for the shipped fan topology,
  pressure strategy, and fan relationship rules.
- `docs/response-evaluation-tuning-plan.md` for any response/tuning evaluation
  pass.
- `docs/RUNTIME_LOGGING_AND_EVALUATION.md` for the analyzer workflow.
- `docs/features/FEAT-0020-standard-control-loop-power-logging.md` for standard
  CPU/GPU power logging.
- `docs/features/FEAT-0021-standard-control-loop-gpu-workload-context-logging.md`
  for cached GPU utilization/clock/VRAM context logging.
- `docs/power-temp-comparison-snapshot-2026-06-18.md` for the current preserved
  CPU/GPU watts plus temperature comparison snapshot.

Closed conclusion preserved from the original:

- No cadence or curve tuning should be derived from this stale discovery data.
  Use pinned, closed archives and the current analyzer/report workflow.
