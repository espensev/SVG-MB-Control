# Modular Profile Hot-Swap - Discussion

**Date:** 2026-06-06
**Status:** compacted historical design commentary, 2026-06-20.

This file is a closed discussion record for FEAT-0003. It is not an
implementation plan and does not authorize product-code work.

Authoritative current surfaces:

- `docs/features/FEAT-0003-selectable-profile-hot-swap.md` is the spec of record.
  It remains `Draft` and design-capture only.
- `docs/profile-hot-swap-decision-2026-06-03.md` is the decision record for the
  control-law seam, state decoupling, PID shape, profile swap, and live-gate
  posture.
- `docs/MEASUREMENT_GATE.md` defines the evidence gate for any controller
  strategy that can change live behavior.
- `docs/features/FEAT-0001-hot-swap-write-policy.md` owns write-policy hot-swap.

Preserved conclusions from the original discussion:

- A swappable control-law seam would be a large demonstration, not scheduled work.
- PID is recorded as a stress-test example for the abstraction, not as a desired
  near-term control strategy.
- Any live PID or alternative control law would cross the measurement gate.
  Shadow/dry-run evidence and an explicit `allow_live` gate would not replace the
  need to characterize live behavior before moving the shipped baseline.
- The safest dependency order remains seam extraction with byte-identical
  curve-overlay behavior before any alternate law or runtime profile swap.

No `REQ-*` rows are introduced here. If FEAT-0003 is ever promoted from `Draft`,
update the feature spec, `docs/TRACEABILITY.md`, `docs/CONTROL_PIPELINE_MATH.md`,
`docs/RUNTIME_HOME.md`, and the operator docs in the same change.
