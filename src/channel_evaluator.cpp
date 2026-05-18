#include "channel_evaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
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
                         double fall_rate_pct_per_min) {
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
    if (std::abs(delta) <= allowed) {
        return desired_pct;
    }
    return last_pct + (delta > 0.0 ? allowed : -allowed);
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

double UpdateThermalPressureBoost(double observed_temp_c,
                                  double current_boost_pct,
                                  std::uint64_t elapsed_ms,
                                  const ChannelControlConfig& config) {
    if (std::isnan(config.thermal_pressure_start_c) ||
        std::isnan(config.thermal_pressure_full_c) ||
        std::isnan(config.thermal_pressure_rise_pct_per_sec) ||
        std::isnan(config.thermal_pressure_fall_pct_per_sec) ||
        std::isnan(config.thermal_pressure_max_boost_pct) ||
        config.thermal_pressure_rise_pct_per_sec <= 0.0 ||
        config.thermal_pressure_fall_pct_per_sec < 0.0 ||
        config.thermal_pressure_max_boost_pct <= 0.0) {
        return 0.0;
    }

    double boost = std::isnan(current_boost_pct)
        ? 0.0
        : std::clamp(
              current_boost_pct, 0.0, config.thermal_pressure_max_boost_pct);
    if (elapsed_ms == 0u || std::isnan(observed_temp_c)) {
        return boost;
    }

    const double dt_seconds = static_cast<double>(elapsed_ms) / 1000.0;
    if (observed_temp_c >= config.thermal_pressure_start_c) {
        double pressure_scale = 1.0;
        if (config.thermal_pressure_full_c >
            config.thermal_pressure_start_c) {
            double t = std::clamp(
                (observed_temp_c - config.thermal_pressure_start_c) /
                    (config.thermal_pressure_full_c -
                     config.thermal_pressure_start_c),
                0.0, 1.0);
            pressure_scale = t * t * t * ((6.0 * t - 15.0) * t + 10.0);
        }

        if (boost < config.thermal_pressure_max_boost_pct) {
            boost += config.thermal_pressure_rise_pct_per_sec *
                     pressure_scale * dt_seconds;
        }
    } else {
        boost -= config.thermal_pressure_fall_pct_per_sec * dt_seconds;
    }

    return std::clamp(boost, 0.0, config.thermal_pressure_max_boost_pct);
}

double UpdateCpuLowSoakBoost(double cpu_temp_c,
                             bool cpu_available,
                             double current_boost_pct,
                             std::uint64_t elapsed_ms,
                             const ChannelControlConfig& config) {
    if (std::isnan(config.cpu_low_soak_start_c) ||
        std::isnan(config.cpu_low_soak_full_c) ||
        std::isnan(config.cpu_low_soak_release_c) ||
        std::isnan(config.cpu_low_soak_rise_pct_per_min) ||
        std::isnan(config.cpu_low_soak_fall_pct_per_min) ||
        std::isnan(config.cpu_low_soak_max_boost_pct) ||
        config.cpu_low_soak_rise_pct_per_min <= 0.0 ||
        config.cpu_low_soak_fall_pct_per_min < 0.0 ||
        config.cpu_low_soak_max_boost_pct <= 0.0) {
        return 0.0;
    }

    double boost = std::isnan(current_boost_pct)
        ? 0.0
        : std::clamp(
              current_boost_pct, 0.0, config.cpu_low_soak_max_boost_pct);
    if (elapsed_ms == 0u) {
        return boost;
    }

    const double dt_minutes = static_cast<double>(elapsed_ms) / 60000.0;
    if (cpu_available && !std::isnan(cpu_temp_c)) {
        if (cpu_temp_c >= config.cpu_low_soak_start_c) {
            const double t = std::clamp(
                (cpu_temp_c - config.cpu_low_soak_start_c) /
                    (config.cpu_low_soak_full_c -
                     config.cpu_low_soak_start_c),
                0.0, 1.0);
            const double soak_scale =
                t * t * t * ((6.0 * t - 15.0) * t + 10.0);
            boost += config.cpu_low_soak_rise_pct_per_min *
                     soak_scale * dt_minutes;
        } else if (cpu_temp_c <= config.cpu_low_soak_release_c) {
            boost -= config.cpu_low_soak_fall_pct_per_min * dt_minutes;
        }
    } else {
        boost -= config.cpu_low_soak_fall_pct_per_min * dt_minutes;
    }

    return std::clamp(boost, 0.0, config.cpu_low_soak_max_boost_pct);
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

std::string AddResponseModifier(std::string source, const char* modifier) {
    if (source.empty() || source == "unavailable") {
        return modifier;
    }
    source += '+';
    source += modifier;
    return source;
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

    const double blended = BlendTemps(temp_inputs, channel.config.temp_blend);
    const bool primary_available = blended >= -100.0;
    double raw_desired_setpoint = std::numeric_limits<double>::quiet_NaN();
    double observed_temp_c = std::numeric_limits<double>::quiet_NaN();
    std::string response_source = "unavailable";

    if (!primary_available) {
        ++channel.consecutive_sensor_failures;
        if (channel.consecutive_sensor_failures >=
            ChannelState::kMaxConsecutiveSensorFailures) {
            if (!channel.sensor_failed) {
                channel.sensor_failed = true;
                evaluation.sensor_event =
                    ChannelSensorEvent::FailureDetected;
            }
            raw_desired_setpoint = ChannelState::kSafeModeFanDuty;
            response_source = "sensor_safe_mode";
        }
    } else {
        if (channel.consecutive_sensor_failures > 0u ||
            channel.sensor_failed) {
            if (channel.sensor_failed) {
                evaluation.sensor_event = ChannelSensorEvent::Recovered;
                evaluation.sensor_event_observed_temp_c = blended;
            }
            channel.consecutive_sensor_failures = 0u;
            channel.sensor_failed = false;
        }

        raw_desired_setpoint = LookupCurve(
            channel.config.curve, blended, channel.config.min_duty_pct,
            channel.config.curve_shape);
        observed_temp_c = blended;
        response_source = "primary_curve";
    }

    if (!channel.config.cpu_override_curve.empty() &&
        temp_inputs.cpu_available) {
        const double cpu_setpoint = LookupCurve(
            channel.config.cpu_override_curve, temp_inputs.cpu_c,
            channel.config.min_duty_pct, channel.config.curve_shape);
        if (std::isnan(raw_desired_setpoint) ||
            cpu_setpoint > raw_desired_setpoint) {
            raw_desired_setpoint = cpu_setpoint;
            observed_temp_c = temp_inputs.cpu_c;
            response_source = "cpu_override";
        }
    }

    channel.last_observed_temp_c = observed_temp_c;
    channel.last_raw_demand_pct = raw_desired_setpoint;
    evaluation.observed_temp_c = observed_temp_c;
    evaluation.response_source = response_source;
    if (std::isnan(raw_desired_setpoint)) {
        channel.last_response_source = response_source;
        return evaluation;
    }

    const double smoothed_base_setpoint = ApplyDemandSmoothing(
        raw_desired_setpoint, channel.smoothed_demand_pct,
        evaluation.timing.elapsed_since_last_evaluation_ms, channel.config);
    channel.smoothed_demand_pct = smoothed_base_setpoint;
    channel.thermal_pressure_boost_pct = UpdateThermalPressureBoost(
        observed_temp_c, channel.thermal_pressure_boost_pct,
        evaluation.timing.elapsed_since_last_evaluation_ms, channel.config);
    channel.cpu_low_soak_boost_pct = UpdateCpuLowSoakBoost(
        temp_inputs.cpu_c, temp_inputs.cpu_available,
        channel.cpu_low_soak_boost_pct,
        evaluation.timing.elapsed_since_last_evaluation_ms, channel.config);
    if (channel.thermal_pressure_boost_pct > 0.0005) {
        response_source =
            AddResponseModifier(std::move(response_source), "thermal_pressure");
    }
    if (channel.cpu_low_soak_boost_pct > 0.0005) {
        response_source =
            AddResponseModifier(std::move(response_source), "cpu_low_soak");
    }
    channel.last_response_source = response_source;
    evaluation.response_source = response_source;

    const double desired_setpoint = std::clamp(
        smoothed_base_setpoint + channel.thermal_pressure_boost_pct +
            channel.cpu_low_soak_boost_pct,
        0.0, 100.0);
    const double setpoint = RateLimitSetpoint(
        desired_setpoint, channel.last_issued_pct,
        evaluation.timing.elapsed_since_last_write_ms,
        channel.config.rise_rate_pct_per_min,
        channel.config.fall_rate_pct_per_min);
    channel.last_setpoint_pct = setpoint;

    if (const RuntimeFanSnapshot* fan = FindRuntimeFanChannel(
            runtime_snapshot, channel.config.channel)) {
        evaluation.authority_reassert =
            evaluation.timing.effective_hold_ms == 0u &&
            FanNeedsAuthorityReassert(
                *fan, channel.last_issued_pct,
                (std::max)(kAuthorityDutyTolerancePct,
                           evaluation.timing.effective_deadband_pct),
                &evaluation.authority_detail);
    }

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
