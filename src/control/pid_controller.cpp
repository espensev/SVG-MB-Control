#include "pid_controller.h"

#include "channel_evaluator.h"  // BuildChannelTimingConfig, SelectPrimaryCurveInput, RateLimitSetpoint
#include "control_policy.h"     // LookupCurve

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>

namespace svg_mb_control {

namespace {

// Clamp dt against the documented multi-second control-loop stalls
// (cpu-loop-stall reproduction) so a single long tick cannot produce a giant
// integral jump or derivative spike when PID drives the loop live.
constexpr double kPidMaxDtSeconds = 5.0;

constexpr char kPidKind[] = "pid";

}  // namespace

PidTerms PidStep(const PidGains& gains, double bias_pct,
                 double observed_temp_c, double target_c, double dt_s,
                 double output_min_pct, double output_max_pct,
                 double integral_min, double integral_max, PidState& state) {
    PidTerms terms;
    if (!std::isfinite(observed_temp_c) || !std::isfinite(target_c)) {
        // A non-finite measurement (sensor glitch) or target must not poison the
        // persistent integral: NaN propagates through error*dt and survives every
        // clamp/saturation comparison (all false for NaN), permanently corrupting
        // the accumulator. Freeze the integral + derivative memory and return a
        // non-finite raw setpoint so the caller's clamp/safe handling takes over.
        state.has_prev = false;
        terms.raw_setpoint_pct = std::numeric_limits<double>::quiet_NaN();
        return terms;
    }
    const double error = observed_temp_c - target_c;
    terms.error_c = error;
    terms.p_term = gains.kp * error;

    // Derivative-on-measurement, positive sign: a rising temperature raises duty.
    double d_term = 0.0;
    if (state.has_prev && dt_s > 0.0 && gains.kd != 0.0) {
        d_term = gains.kd * (observed_temp_c - state.prev_temp_c) / dt_s;
    }
    terms.d_term = d_term;

    // Tentative integration with a clamped accumulator (anti-windup part 1).
    const bool integrating = state.has_prev && dt_s > 0.0 && gains.ki != 0.0;
    double candidate_integral = state.integral;
    if (integrating) {
        candidate_integral = state.integral + error * dt_s;
        if (!std::isnan(integral_min) && candidate_integral < integral_min) {
            candidate_integral = integral_min;
        }
        if (!std::isnan(integral_max) && candidate_integral > integral_max) {
            candidate_integral = integral_max;
        }
    }
    double raw = bias_pct + terms.p_term + gains.ki * candidate_integral + d_term;

    // Conditional integration (anti-windup part 2): if the raw output saturates
    // in the direction the error pushes, do not commit this step's integration.
    bool commit = true;
    if (integrating) {
        if (raw > output_max_pct && error > 0.0) {
            commit = false;
        } else if (raw < output_min_pct && error < 0.0) {
            commit = false;
        }
    }
    if (commit) {
        state.integral = candidate_integral;
    } else {
        raw = bias_pct + terms.p_term + gains.ki * state.integral + d_term;
    }
    terms.i_term = gains.ki * state.integral;
    terms.raw_setpoint_pct = raw;

    state.prev_temp_c = observed_temp_c;
    state.has_prev = true;
    return terms;
}

bool PidLiveAuthorized(const ChannelControlConfig& config, std::string& reason) {
    const auto& pid = config.pid;
    if (!pid.allow_live) {
        reason = "pid.allow_live not set; running shadow/dry-run";
        return false;
    }
    const bool has_step_cap = !std::isnan(config.max_setpoint_step_pct) &&
                              config.max_setpoint_step_pct > 0.0;
    const bool has_rate_cap = !std::isnan(config.rise_rate_pct_per_min) &&
                              config.rise_rate_pct_per_min > 0.0 &&
                              !std::isnan(config.fall_rate_pct_per_min) &&
                              config.fall_rate_pct_per_min > 0.0;
    if (!has_step_cap && !has_rate_cap) {
        reason =
            "pid.allow_live requires a positive slew cap "
            "(max_setpoint_step_pct, or both rise/fall rates)";
        return false;
    }
    if (pid.characterization_artifact.empty()) {
        reason = "pid.allow_live requires a characterization_artifact path";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(pid.characterization_artifact, ec) || ec) {
        reason = "pid.allow_live characterization artifact not found: " +
                 pid.characterization_artifact;
        return false;
    }
    reason.clear();
    return true;
}

PidController::PidController(const ChannelControlConfig& config)
    : config_(config) {
    live_ = PidLiveAuthorized(config_, shadow_reason_);
}

ChannelEvaluation PidController::Evaluate(
    ChannelState& channel, const ControlLoopConfig& loop,
    const TempInputs& temp_inputs, const RuntimeSnapshotIndex& runtime_index,
    std::chrono::steady_clock::time_point now) {
    (void)runtime_index;  // PID reads temps from temp_inputs (prepped per tick).

    ChannelEvaluation evaluation;
    evaluation.timing = BuildChannelTimingConfig(loop, channel, now);
    channel.last_evaluation_time = now;
    channel.last_write_reason = "none";
    // A non-live PID channel is shadow/dry-run: compute + log, never actuate.
    evaluation.write_suppressed = !live_;

    const PrimaryCurveInput primary =
        SelectPrimaryCurveInput(temp_inputs, channel.config);
    channel.last_primary_temp_source = primary.source;

    if (!primary.available) {
        // Shared sensor-safe mode (REQ-PROFILE-06): drive the safe duty and flag
        // safety_override so a LIVE PID's safe command bypasses the write-failure
        // breaker. Freeze PID memory while the primary sensor is blind.
        state_.has_prev = false;
        const double safe = ChannelState::kSafeModeFanDuty;
        channel.last_observed_temp_c = std::numeric_limits<double>::quiet_NaN();
        channel.last_setpoint_pct = safe;
        channel.last_response_source = "sensor_safe_mode";
        channel.pid_error_c = std::numeric_limits<double>::quiet_NaN();
        channel.pid_p_term = std::numeric_limits<double>::quiet_NaN();
        channel.pid_i_term = std::numeric_limits<double>::quiet_NaN();
        channel.pid_d_term = std::numeric_limits<double>::quiet_NaN();
        channel.pid_setpoint_raw_pct = std::numeric_limits<double>::quiet_NaN();
        evaluation.has_setpoint = true;
        evaluation.observed_temp_c = std::numeric_limits<double>::quiet_NaN();
        evaluation.setpoint_pct = safe;
        evaluation.response_source = "sensor_safe_mode";
        evaluation.safety_override = true;
        return evaluation;
    }

    double dt_s =
        static_cast<double>(evaluation.timing.elapsed_since_last_evaluation_ms) /
        1000.0;
    if (dt_s < 0.0) dt_s = 0.0;
    if (dt_s > kPidMaxDtSeconds) dt_s = kPidMaxDtSeconds;

    const auto& pid = config_.pid;
    const double bias =
        pid.feedforward == PidFeedforward::Fixed
            ? pid.fixed_feedforward_pct
            : LookupCurve(channel.config.curve, primary.temp_c,
                          channel.config.min_duty_pct,
                          channel.config.curve_shape);

    const PidGains gains{pid.kp, pid.ki, pid.kd};
    const double out_min = std::clamp(channel.config.min_duty_pct, 0.0, 100.0);
    const PidTerms terms =
        PidStep(gains, bias, primary.temp_c, pid.target_c, dt_s, out_min, 100.0,
                pid.integral_min, pid.integral_max, state_);

    double setpoint = std::clamp(terms.raw_setpoint_pct, out_min, 100.0);
    setpoint = RateLimitSetpoint(setpoint, channel.last_issued_pct,
                                 evaluation.timing.elapsed_since_last_write_ms,
                                 channel.config.rise_rate_pct_per_min,
                                 channel.config.fall_rate_pct_per_min,
                                 channel.config.max_setpoint_step_pct);

    // Record the trajectory + PID evidence on ChannelState (the CSV/status log
    // reads these). last_raw_demand_pct stays NaN: it is the curve law's
    // pre-smoothing demand published as feedforward_pct, which must blank for PID.
    channel.last_observed_temp_c = primary.temp_c;
    channel.last_setpoint_pct = setpoint;
    channel.last_response_source = kPidKind;
    channel.pid_error_c = terms.error_c;
    channel.pid_p_term = terms.p_term;
    channel.pid_i_term = terms.i_term;
    channel.pid_d_term = terms.d_term;
    channel.pid_setpoint_raw_pct = terms.raw_setpoint_pct;

    evaluation.has_setpoint = true;
    evaluation.observed_temp_c = primary.temp_c;
    evaluation.setpoint_pct = setpoint;
    evaluation.response_source = kPidKind;
    return evaluation;
}

void PidController::Reset() { state_ = PidState{}; }

std::string_view PidController::Kind() const { return kPidKind; }

ControllerStartupInfo PidController::StartupInfo() const {
    ControllerStartupInfo info;
    info.is_pid = true;
    info.live = live_;
    info.detail =
        live_ ? std::string("law=pid live (allow_live evidenced + slew cap)")
              : shadow_reason_;
    return info;
}

}  // namespace svg_mb_control
