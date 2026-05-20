#pragma once

#include "control_loop.h"
#include "control_policy.h"

#include <cstdint>
#include <limits>

namespace svg_mb_control {

// Quintic smootherstep, clamping input to [0, 1] before evaluation.
double SmoothStep(double t);

// Maps `value` to [0, 1] via SmoothStep across [start, full]. Returns 0
// when value is NaN or when full <= start (band collapsed / inert).
double SmoothScale(double value, double start, double full);

// Rate-limited step from `current` toward `target`. Used by the cadence
// integrator (asymmetric rise/fall) and by the low-band stage-boost
// integrator. NaN `current` is treated as 0.0; non-positive `dt_minutes`
// snaps to `target`; NaN or non-positive direction-rate snaps to
// `target` (no rate limit applied).
double MoveTowardRateLimited(double current,
                             double target,
                             double dt_minutes,
                             double rise_per_min,
                             double fall_per_min);

// Upward-only adaptive cadence (design: docs/adaptive-cadence-design-
// 2026-05-19.md, Phase 2). Slew-only: the setpoint-motion transient term
// is deferred to Phase 2b (its scale is undefined in the design). State
// carried across ticks for the slew delta and the rate-limited effective
// interval.
struct CadenceRuntimeState {
    double prev_cpu_c = std::numeric_limits<double>::quiet_NaN();
    double prev_gpu_c = std::numeric_limits<double>::quiet_NaN();
    double effective_ms = std::numeric_limits<double>::quiet_NaN();
};

struct CadenceTick {
    std::uint32_t effective_interval_ms = 0u;
    double transient = 0.0;
};

// Returns the effective interval E in [F, P] for the iteration just
// completed and the unitless transient that produced it. F >= P (the
// default when poll_tick_floor_ms is absent) pins E == P every tick, so
// the loop is byte-identical to the pre-feature path.
CadenceTick ComputeCadence(const ControlLoopConfig& cfg,
                           const TempInputs& temp_inputs,
                           double dt_ms,
                           CadenceRuntimeState& state);

}  // namespace svg_mb_control
