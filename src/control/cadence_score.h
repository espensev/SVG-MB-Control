#pragma once

#include "control_loop.h"
#include "control_policy.h"

#include <cstdint>
#include <limits>

namespace svg_mb_control {

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
