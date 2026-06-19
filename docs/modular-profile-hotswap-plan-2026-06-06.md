# Modular Profile Hot-Swap - Deepening Plan

**Date:** 2026-06-06
**Status:** compacted historical design-capture, 2026-06-20.

This file is a short pointer plus the small reference tables that remain useful
from the original deepening plan. It does not authorize implementation.
FEAT-0003 stays `Draft` and design-capture only.

Authoritative current surfaces:

- `docs/features/FEAT-0003-selectable-profile-hot-swap.md` is the spec of record.
- `docs/profile-hot-swap-decision-2026-06-03.md` records the selected design
  directions for a hypothetical demonstration.
- `docs/MEASUREMENT_GATE.md` governs any live controller-strategy change.
- `docs/features/README.md` and `docs/TRACEABILITY.md` define promotion and
  verification requirements.

## Current Curve-Overlay Stages

These stages describe the current control law that any future controller seam
would have to preserve byte-for-byte before adding another law:

| Stage | What it does | Current home |
|---|---|---|
| Input select + primary curve + sensor-failure latch | Selects the primary temperature input, applies the curve, and forces sensor-safe behavior after sustained input failure. | `src/control/channel_evaluator.cpp` |
| CPU override curve | Strict max-wins overlay; cannot lower demand. | `src/control/channel_evaluator.cpp` |
| Demand smoothing | Asymmetric rise/fall smoothing plus decay-latch floor. | `src/control/channel_evaluator.cpp` |
| Boost overlays | Thermal-pressure, midband-pressure, GPU-airflow, and CPU-low-soak leaky integrators. | `src/control/boost_stage.cpp`, `src/control/channel_evaluator.cpp` |
| Low-band cap + final sum + clamp | Caps low-band residual, sums base plus boosts plus residual, clamps to `[0,100]`. | `src/control/channel_evaluator.cpp` |
| Rate limiter | Bounds movement from the last issued duty by rate and step caps. | `src/control/channel_evaluator.cpp` |
| Authority reassert | Detects observed fan-state drift during continuous-hold mode. | `src/control/channel_evaluator.cpp` |

## Reusable Hot-Swap Machinery

| Primitive | Status | Current home |
|---|---|---|
| `CreateFanWriter(runtime_policy)` factory | implemented | `src/hardware/fan_writer.h` |
| Expired hold restore then clear `write_active` | implemented | `src/control/channel_write.cpp` |
| Breaker-reset request intake pattern | implemented | `src/runtime/runtime_lifecycle.cpp`, `src/control/tick_runner.cpp` |
| `IChannelController` seam / `MakeChannelController` factory | missing | FEAT-0003 only |
| Per-channel controller discriminator + per-law parse | missing | FEAT-0003 only |
| Profile request file + runtime request intake | missing | FEAT-0003 only |
| FEAT-0001 write-policy swap for channel-set changes | spec-only | `docs/features/FEAT-0001-hot-swap-write-policy.md` |

## Dependency Order If Reopened

1. Reconfirm FEAT-0003 is still wanted. The maintained status says it is not a
   net-benefit target and is not scheduled.
2. Promote the spec through the feature gate before product-code work.
3. Extract the current curve-overlay law behind a seam with output identity tests.
4. Only after identity is proven, add any alternate law in shadow/dry-run form.
5. Treat live alternate-law writes as measurement-gated.
