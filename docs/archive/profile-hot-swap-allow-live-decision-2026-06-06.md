# Profile Hot-Swap `pid.allow_live` Reconsideration Brief

**Date:** 2026-06-06
**Status:** compacted resolved brief, 2026-06-20.

This file is the historical support record for the revised D6 in
`docs/profile-hot-swap-decision-2026-06-03.md`. The decision now lives there; do
not treat this brief as a second source of truth.

Resolved outcome:

- The maintainer selected B1 + B2.
- `pid.allow_live: true` would require characterization evidence before live PID
  writes.
- `pid.allow_live: true` would also require a non-NaN positive slew bound at
  config load.
- Shadow/dry-run remains the default for PID or other alternate laws.

Context:

- FEAT-0003 remains `Draft` and design-capture only.
- None of the PID/profile/`allow_live` runtime machinery exists in code.
- Any future implementation must update `docs/features/FEAT-0003-selectable-profile-hot-swap.md`,
  `docs/TRACEABILITY.md`, `docs/MEASUREMENT_GATE.md`, and the runtime/operator
  docs in the same change.
