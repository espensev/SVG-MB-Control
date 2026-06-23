#pragma once

#include "control_runtime_context.h"
#include "runtime_snapshot.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

namespace svg_mb_control {

constexpr std::uint32_t kAuthorityReassertCooldownMs = 2000u;
constexpr double kAuthorityDutyTolerancePct = 3.0;

struct ChannelTimingConfig {
    std::uint32_t effective_cooldown_ms = 0u;
    double effective_deadband_pct = 0.0;
    std::uint32_t effective_hold_ms = 0u;
    std::uint64_t elapsed_since_last_write_ms = 0u;
    std::uint64_t elapsed_since_last_evaluation_ms = 0u;
};

enum class ChannelSensorEvent {
    None,
    FailureDetected,
    Recovered,
};

struct ChannelEvaluation {
    ChannelTimingConfig timing;
    bool has_setpoint = false;
    double observed_temp_c = std::numeric_limits<double>::quiet_NaN();
    double setpoint_pct = std::numeric_limits<double>::quiet_NaN();
    std::string response_source = "unavailable";
    bool authority_reassert = false;
    std::string authority_detail;
    ChannelSensorEvent sensor_event = ChannelSensorEvent::None;
    double sensor_event_observed_temp_c =
        std::numeric_limits<double>::quiet_NaN();
    // True when this evaluation is the sensor-failure safe-mode command
    // (setpoint forced to kSafeModeFanDuty because the primary sensor is
    // unavailable). The write path lets this thermal-safety command bypass
    // the write-failure circuit breaker so it is not silently suppressed
    // (docs/discovery-recovery-gap-audit-2026-06-04.md, remediation 3).
    bool safety_override = false;
    // FEAT-0003 (REQ-PROFILE-07): shadow/dry-run. When true, the law computed and
    // logged this setpoint but the channel must NOT actuate or assert authority --
    // the write path early-returns before any fan write. Set by a non-live PID
    // controller (pid.allow_live absent/false, or downgraded for missing evidence
    // or slew cap). The curve law always leaves this false. Distinct from
    // has_setpoint=false so the shadow trajectory still reaches the CSV/status log.
    bool write_suppressed = false;
};

// Primary control-temperature selection result: the channel's measured temp and
// a human-readable source label. Shared by the curve law and the PID law so both
// pick the temperature the same way, including the source-aware guard (FEAT-0003
// decision D3). Hoisted from channel_evaluator.cpp's anonymous namespace so the
// PID controller in another translation unit can reuse it.
struct PrimaryCurveInput {
    bool available = false;
    double temp_c = std::numeric_limits<double>::quiet_NaN();
    std::string source = "unavailable";
};

// Selects the primary control temperature for config.temp_blend (cpu_only,
// gpu_only, max, source-aware). SelectLegacyMaxInput is the max/guarded helper.
PrimaryCurveInput SelectPrimaryCurveInput(const TempInputs& inputs,
                                          const ChannelControlConfig& config);
PrimaryCurveInput SelectLegacyMaxInput(const TempInputs& inputs, bool guarded);

// Safety slew clamp on a freshly computed setpoint: bounds the per-tick change by
// two INDEPENDENT limits -- the rise/fall rate (percent per minute, scaled by
// elapsed) and the absolute max_setpoint_step_pct -- and the tighter one wins. A
// NaN last_pct (no prior write) means no limit; otherwise the step cap still
// applies even when the directional rate is NaN/<=0 (and vice versa). Only when
// neither bound is set is the move unlimited. Shared by both laws (decision D4: a
// mis-tuned PID must not be able to step the duty arbitrarily fast).
double RateLimitSetpoint(double desired_pct, double last_pct,
                         std::uint64_t elapsed_ms,
                         double rise_rate_pct_per_min,
                         double fall_rate_pct_per_min,
                         double max_setpoint_step_pct);

double GpuControlEnvelopeC(const RuntimeGpuSnapshot& gpu);

ChannelTimingConfig BuildChannelTimingConfig(
    const ControlLoopConfig& loop,
    const ChannelState& channel,
    std::chrono::steady_clock::time_point now);

ChannelEvaluation EvaluateChannel(ChannelState& channel,
                                  const ControlLoopConfig& loop,
                                  const TempInputs& temp_inputs,
                                  const RuntimeSnapshotIndex& runtime_index,
                                  std::chrono::steady_clock::time_point now);

std::uint32_t WriteCooldownForAuthorityReassert(
    const ChannelTimingConfig& timing,
    bool authority_reassert);

}  // namespace svg_mb_control
