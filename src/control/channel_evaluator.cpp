#include "channel_evaluator.h"

#include "boost_stage.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace svg_mb_control {

namespace {

std::uint32_t EffectiveWriteCooldownMs(const ControlLoopConfig& loop,
                                       const ChannelControlConfig& channel) {
    return channel.write_cooldown_ms > 0u
        ? channel.write_cooldown_ms
        : loop.write_cooldown_ms;
}

double EffectiveDeadbandPct(const ControlLoopConfig& loop,
                            const ChannelControlConfig& channel) {
    return std::isnan(channel.deadband_pct)
        ? loop.deadband_pct
        : channel.deadband_pct;
}

std::uint32_t EffectiveControlHoldMs(const ControlLoopConfig& loop,
                                     const ChannelControlConfig& channel) {
    return channel.control_hold_ms > 0u
        ? channel.control_hold_ms
        : loop.control_hold_ms;
}

double RateLimitSetpoint(double desired_pct,
                         double last_pct,
                         std::uint64_t elapsed_ms,
                         double rise_rate_pct_per_min,
                         double fall_rate_pct_per_min,
                         double max_setpoint_step_pct) {
    if (std::isnan(last_pct)) {
        return desired_pct;
    }

    const double delta = desired_pct - last_pct;
    if (std::abs(delta) <= 0.0001) {
        return desired_pct;
    }

    const double rate = delta > 0.0
        ? rise_rate_pct_per_min
        : fall_rate_pct_per_min;
    if (std::isnan(rate) || rate <= 0.0) {
        return desired_pct;
    }

    const double allowed =
        rate * (static_cast<double>(elapsed_ms) / 60000.0);
    double max_allowed = allowed;
    if (!std::isnan(max_setpoint_step_pct) && max_setpoint_step_pct > 0.0) {
        max_allowed = (std::min)(max_allowed, max_setpoint_step_pct);
    }

    if (std::abs(delta) <= max_allowed) {
        return desired_pct;
    }
    return last_pct + (delta > 0.0 ? max_allowed : -max_allowed);
}

double ApplyDemandSmoothing(double raw_desired_pct,
                            double last_smoothed_pct,
                            std::uint64_t elapsed_ms,
                            const ChannelControlConfig& config) {
    if (std::isnan(last_smoothed_pct)) {
        return raw_desired_pct;
    }

    const double delta = raw_desired_pct - last_smoothed_pct;
    if (std::abs(delta) <= 0.0001) {
        return raw_desired_pct;
    }

    if (delta > 0.0) {
        if (std::isnan(config.demand_smoothing_rise_alpha)) {
            return raw_desired_pct;
        }
        const double alpha =
            std::clamp(config.demand_smoothing_rise_alpha, 0.0, 1.0);
        return std::clamp(last_smoothed_pct + alpha * delta, 0.0, 100.0);
    }

    double smoothed = raw_desired_pct;
    if (!std::isnan(config.demand_smoothing_fall_alpha)) {
        const double alpha =
            std::clamp(config.demand_smoothing_fall_alpha, 0.0, 1.0);
        smoothed = last_smoothed_pct + alpha * delta;
    }

    if (!std::isnan(config.decay_latch_pct_per_min) &&
        config.decay_latch_pct_per_min > 0.0 &&
        elapsed_ms > 0u) {
        const bool latch_zone =
            std::isnan(config.decay_latch_above_pct) ||
            last_smoothed_pct >= config.decay_latch_above_pct ||
            smoothed >= config.decay_latch_above_pct;
        if (latch_zone) {
            const double max_drop =
                config.decay_latch_pct_per_min *
                (static_cast<double>(elapsed_ms) / 60000.0);
            smoothed = (std::max)(smoothed, last_smoothed_pct - max_drop);
        }
    }

    return std::clamp(smoothed, 0.0, 100.0);
}

bool FanNeedsAuthorityReassert(const RuntimeFanSnapshot& fan,
                               double last_issued_pct,
                               double tolerance_pct,
                               std::string* detail) {
    if (std::isnan(last_issued_pct)) {
        return false;
    }

    const bool mode_drifted = fan.mode_raw != 0u;
    const double duty_delta = std::abs(fan.duty_percent - last_issued_pct);
    const bool duty_drifted = duty_delta > tolerance_pct;
    if (!mode_drifted && !duty_drifted) {
        return false;
    }

    if (detail != nullptr) {
        std::ostringstream stream;
        stream << "observed fan state drifted from last issued setpoint"
               << " mode_raw=" << static_cast<unsigned int>(fan.mode_raw)
               << " duty_pct=" << fan.duty_percent
               << " last_issued_pct=" << last_issued_pct
               << " tolerance_pct=" << tolerance_pct;
        *detail = stream.str();
    }
    return true;
}

std::string AddResponseModifier(std::string source,
                                std::string_view modifier) {
    if (source.empty() || source == "unavailable") {
        return std::string(modifier);
    }
    source += '+';
    source.append(modifier);
    return source;
}

struct PrimaryCurveInput {
    bool available = false;
    double temp_c = std::numeric_limits<double>::quiet_NaN();
    std::string source = "unavailable";
};

PrimaryCurveInput SelectLegacyMaxInput(const TempInputs& inputs,
                                       bool guarded) {
    PrimaryCurveInput out;
    if (!inputs.cpu_available && !inputs.gpu_available) {
        return out;
    }
    if (inputs.cpu_available &&
        (!inputs.gpu_available || inputs.cpu_c >= inputs.gpu_c)) {
        out.available = true;
        out.temp_c = inputs.cpu_c;
        out.source = guarded ? "cpu_guard" : "cpu";
        return out;
    }
    out.available = true;
    out.temp_c = inputs.gpu_c;
    out.source = guarded ? "gpu_guard" : "gpu";
    return out;
}

// Per-evaluation scratch state threaded through the EvaluateChannel
// helpers. References capture the call inputs; the four in-progress
// fields carry partial results between helpers. Pure scratch — does not
// outlive the EvaluateChannel call.
struct EvaluationScratch {
    ChannelState& channel;
    const ControlLoopConfig& loop;
    const TempInputs& temp_inputs;
    const RuntimeSnapshot& runtime_snapshot;
    ChannelEvaluation& evaluation;

    double raw_desired_setpoint = std::numeric_limits<double>::quiet_NaN();
    double observed_temp_c = std::numeric_limits<double>::quiet_NaN();
    std::string response_source = "unavailable";
    std::string primary_temp_source = "unavailable";
};

PrimaryCurveInput SelectPrimaryCurveInput(
    const TempInputs& inputs,
    const ChannelControlConfig& config) {
    PrimaryCurveInput out;
    switch (config.temp_blend) {
        case TempBlend::CpuOnly:
            if (inputs.cpu_available) {
                out.available = true;
                out.temp_c = inputs.cpu_c;
                out.source = "cpu";
            }
            return out;
        case TempBlend::GpuOnly:
            if (inputs.gpu_available) {
                out.available = true;
                out.temp_c = inputs.gpu_c;
                out.source = "gpu";
            }
            return out;
        case TempBlend::MaxCpuGpu:
            return SelectLegacyMaxInput(inputs, false);
        case TempBlend::MaxCpuGpuSourceAware:
            if (!std::isnan(config.source_aware_cpu_hot_guard_c) &&
                inputs.cpu_available &&
                inputs.cpu_c >= config.source_aware_cpu_hot_guard_c) {
                return SelectLegacyMaxInput(inputs, true);
            }
            if (inputs.gpu_available) {
                out.available = true;
                out.temp_c = inputs.gpu_c;
                out.source = "gpu";
            } else if (inputs.cpu_available) {
                out.available = true;
                out.temp_c = inputs.cpu_c;
                out.source = "cpu_fallback";
            }
            return out;
    }
    return out;
}

// Step 1 of EvaluateChannel: pick the primary curve input, drive the
// sensor failure/recovery transitions, and seed raw_desired_setpoint
// from the curve. Sets primary_temp_source even on the no-input path so
// callers can record it in channel/evaluation.
void EvaluatePrimarySetpoint(EvaluationScratch& s) {
    const PrimaryCurveInput primary =
        SelectPrimaryCurveInput(s.temp_inputs, s.channel.config);
    s.primary_temp_source = primary.source;

    if (!primary.available) {
        ++s.channel.consecutive_sensor_failures;
        if (s.channel.consecutive_sensor_failures >=
            ChannelState::kMaxConsecutiveSensorFailures) {
            if (!s.channel.sensor_failed) {
                s.channel.sensor_failed = true;
                s.evaluation.sensor_event =
                    ChannelSensorEvent::FailureDetected;
            }
            s.raw_desired_setpoint = ChannelState::kSafeModeFanDuty;
            s.response_source = "sensor_safe_mode";
        }
        return;
    }

    if (s.channel.consecutive_sensor_failures > 0u ||
        s.channel.sensor_failed) {
        if (s.channel.sensor_failed) {
            s.evaluation.sensor_event = ChannelSensorEvent::Recovered;
            s.evaluation.sensor_event_observed_temp_c = primary.temp_c;
        }
        s.channel.consecutive_sensor_failures = 0u;
        s.channel.sensor_failed = false;
    }

    s.raw_desired_setpoint = LookupCurve(
        s.channel.config.curve, primary.temp_c,
        s.channel.config.min_duty_pct, s.channel.config.curve_shape);
    s.observed_temp_c = primary.temp_c;
    s.response_source = "primary_curve";
}

// Step 2: merge the optional CPU override curve. Wins if no primary
// input was available or if its curve commands a higher duty.
void ApplyCpuOverride(EvaluationScratch& s) {
    if (s.channel.config.cpu_override_curve.empty() ||
        !s.temp_inputs.cpu_available) {
        return;
    }
    const double cpu_setpoint = LookupCurve(
        s.channel.config.cpu_override_curve, s.temp_inputs.cpu_c,
        s.channel.config.min_duty_pct, s.channel.config.curve_shape);
    if (std::isnan(s.raw_desired_setpoint) ||
        cpu_setpoint > s.raw_desired_setpoint) {
        s.raw_desired_setpoint = cpu_setpoint;
        s.observed_temp_c = s.temp_inputs.cpu_c;
        s.response_source = "cpu_override";
    }
}

// Step 3: smooth the raw demand, integrate the four boost overlays, and
// build the response_source modifier string. Returns the smoothed base
// setpoint for the final clamp+rate-limit step. Writes the final
// response_source onto channel and evaluation.
double UpdateDemandAndBoosts(EvaluationScratch& s) {
    const double smoothed_base_setpoint = ApplyDemandSmoothing(
        s.raw_desired_setpoint, s.channel.smoothed_demand_pct,
        s.evaluation.timing.elapsed_since_last_evaluation_ms,
        s.channel.config);
    s.channel.smoothed_demand_pct = smoothed_base_setpoint;

    for (std::size_t i = 0; i < kBoostStageCount; ++i) {
        s.channel.boosts[i].boost_pct = UpdateBoostStage(
            kBoostStageSpecs[i], s.channel.config.boosts[i],
            s.channel.boosts[i].boost_pct,
            s.evaluation.timing.elapsed_since_last_evaluation_ms,
            s.temp_inputs, s.observed_temp_c);
    }

    constexpr double kBoostResponseTagThresholdPct = 0.0005;
    for (std::size_t i = 0; i < kBoostStageCount; ++i) {
        if (s.channel.boosts[i].boost_pct > kBoostResponseTagThresholdPct) {
            s.response_source = AddResponseModifier(
                std::move(s.response_source),
                kBoostStageSpecs[i].response_tag);
        }
    }
    if (s.channel.low_band_stage_boost_pct > kBoostResponseTagThresholdPct) {
        s.response_source =
            AddResponseModifier(std::move(s.response_source), "low_band_stage");
    }
    s.channel.last_response_source = s.response_source;
    s.evaluation.response_source = s.response_source;
    return smoothed_base_setpoint;
}

// Step 4: apply the low-band residual cap, sum every contribution, clamp
// to [0, 100], and apply the per-channel rate limiter. Returns the
// rate-limited setpoint. Operand order in the boost-sum is load-bearing
// (see CONTROL_PIPELINE_MATH.md §5): preserving left-to-right
// associativity keeps the result bit-identical to pre-table builds.
double ComputeFinalSetpoint(EvaluationScratch& s,
                            double smoothed_base_setpoint) {
    double low_band_contrib = s.channel.low_band_stage_boost_pct;
    if (!std::isnan(s.loop.low_band_residual_cap_pct)) {
        low_band_contrib =
            (std::min)(low_band_contrib, s.loop.low_band_residual_cap_pct);
    }
    s.channel.low_band_effective_boost_pct = low_band_contrib;

    const double desired_setpoint = std::clamp(
        smoothed_base_setpoint +
            s.channel.boosts[static_cast<std::size_t>(
                BoostStage::ThermalPressure)].boost_pct +
            s.channel.boosts[static_cast<std::size_t>(
                BoostStage::MidbandPressure)].boost_pct +
            s.channel.boosts[static_cast<std::size_t>(
                BoostStage::GpuAirflow)].boost_pct +
            s.channel.boosts[static_cast<std::size_t>(
                BoostStage::CpuLowSoak)].boost_pct +
            low_band_contrib,
        0.0, 100.0);

    return RateLimitSetpoint(
        desired_setpoint, s.channel.last_issued_pct,
        s.evaluation.timing.elapsed_since_last_write_ms,
        s.channel.config.rise_rate_pct_per_min,
        s.channel.config.fall_rate_pct_per_min,
        s.channel.config.max_setpoint_step_pct);
}

// Step 5: when running in continuous-hold mode (effective_hold_ms == 0),
// detect drift between the last issued setpoint and the observed fan
// state and flag an authority-reassert. Leaves evaluation.authority_*
// at their defaults when no fan snapshot is present for this channel.
void DetectAuthorityReassert(EvaluationScratch& s) {
    const RuntimeFanSnapshot* fan = FindRuntimeFanChannel(
        s.runtime_snapshot, s.channel.config.channel);
    if (fan == nullptr) {
        return;
    }
    s.evaluation.authority_reassert =
        s.evaluation.timing.effective_hold_ms == 0u &&
        FanNeedsAuthorityReassert(
            *fan, s.channel.last_issued_pct,
            (std::max)(kAuthorityDutyTolerancePct,
                       s.evaluation.timing.effective_deadband_pct),
            &s.evaluation.authority_detail);
}

}  // namespace

double GpuControlEnvelopeC(const RuntimeGpuSnapshot& gpu) {
    double envelope = (std::max)(gpu.core_c, gpu.memjn_c);
    if (gpu.hotspot_c > 0.0) {
        envelope = (std::max)(envelope, gpu.hotspot_c);
    }
    return envelope;
}

ChannelTimingConfig BuildChannelTimingConfig(
    const ControlLoopConfig& loop,
    const ChannelState& channel,
    std::chrono::steady_clock::time_point now) {
    ChannelTimingConfig timing;
    timing.effective_cooldown_ms =
        EffectiveWriteCooldownMs(loop, channel.config);
    timing.effective_deadband_pct =
        EffectiveDeadbandPct(loop, channel.config);
    timing.effective_hold_ms =
        EffectiveControlHoldMs(loop, channel.config);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - channel.last_write_time).count();
    timing.elapsed_since_last_write_ms =
        elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0u;

    if (channel.last_evaluation_time ==
        std::chrono::steady_clock::time_point{}) {
        timing.elapsed_since_last_evaluation_ms = loop.poll_tick_ms;
    } else {
        const auto eval_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - channel.last_evaluation_time).count();
        timing.elapsed_since_last_evaluation_ms =
            eval_elapsed > 0 ? static_cast<std::uint64_t>(eval_elapsed) : 0u;
    }
    return timing;
}

ChannelEvaluation EvaluateChannel(ChannelState& channel,
                                  const ControlLoopConfig& loop,
                                  const TempInputs& temp_inputs,
                                  const RuntimeSnapshot& runtime_snapshot,
                                  std::chrono::steady_clock::time_point now) {
    ChannelEvaluation evaluation;
    evaluation.timing = BuildChannelTimingConfig(loop, channel, now);
    channel.last_evaluation_time = now;
    channel.last_write_reason = "none";

    EvaluationScratch s{channel, loop, temp_inputs, runtime_snapshot,
                        evaluation};

    EvaluatePrimarySetpoint(s);
    ApplyCpuOverride(s);

    channel.last_observed_temp_c = s.observed_temp_c;
    channel.last_raw_demand_pct = s.raw_desired_setpoint;
    channel.last_primary_temp_source = s.primary_temp_source;
    evaluation.observed_temp_c = s.observed_temp_c;
    evaluation.response_source = s.response_source;
    if (std::isnan(s.raw_desired_setpoint)) {
        channel.last_response_source = s.response_source;
        return evaluation;
    }

    const double smoothed_base_setpoint = UpdateDemandAndBoosts(s);
    const double setpoint = ComputeFinalSetpoint(s, smoothed_base_setpoint);
    channel.last_setpoint_pct = setpoint;
    DetectAuthorityReassert(s);

    evaluation.has_setpoint = true;
    evaluation.setpoint_pct = setpoint;
    return evaluation;
}

std::uint32_t WriteCooldownForAuthorityReassert(
    const ChannelTimingConfig& timing,
    bool authority_reassert) {
    return authority_reassert
        ? (std::min)(timing.effective_cooldown_ms,
                     kAuthorityReassertCooldownMs)
        : timing.effective_cooldown_ms;
}

}  // namespace svg_mb_control
