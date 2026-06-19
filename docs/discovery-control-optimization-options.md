# Discovery - Control Optimization Options

**Date:** 2026-05-19
**Status:** compacted historical record, 2026-06-20.

This discovery note is closed. The original file predated the runtime module
split, the analyzer/reporting work, FEAT-0019, FEAT-0020, FEAT-0021, and
FEAT-0022. Its line anchors and hot-path candidates are no longer reliable as
current guidance.

Use these current records instead:

- `docs/STRUCTURE_AND_STABILITY.md` for the source layout and remaining
  structural polish.
- `docs/control-latency-reduction-design-2026-06-18.md` for the current control
  latency audit and the FEAT-0017/0018/0019 directions.
- `docs/RUNTIME_LOGGING_AND_EVALUATION.md` for runtime evidence and analyzer
  workflow.
- `docs/features/README.md` and `docs/TRACEABILITY.md` for what is buildable and
  what is verified.

Closed conclusions preserved from the original:

- Treat the older optimization register as historical unless a current source
  review confirms the same issue still exists.
- Do not tune cadence or curves from stale discovery data. Use fresh runtime
  evidence and the maintained measurement/evaluation docs.
- Large control-loop extraction remains a polish item, not a standalone feature
  target.
