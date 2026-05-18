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
};

double GpuControlEnvelopeC(const RuntimeGpuSnapshot& gpu);

ChannelTimingConfig BuildChannelTimingConfig(
    const ControlLoopConfig& loop,
    const ChannelState& channel,
    std::chrono::steady_clock::time_point now);

ChannelEvaluation EvaluateChannel(ChannelState& channel,
                                  const ControlLoopConfig& loop,
                                  const TempInputs& temp_inputs,
                                  const RuntimeSnapshot& runtime_snapshot,
                                  std::chrono::steady_clock::time_point now);

std::uint32_t WriteCooldownForAuthorityReassert(
    const ChannelTimingConfig& timing,
    bool authority_reassert);

}  // namespace svg_mb_control
