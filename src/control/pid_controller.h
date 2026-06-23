#pragma once

#include "channel_controller.h"
#include "control_loop.h"

#include <limits>
#include <string>

namespace svg_mb_control {

// Pure PID accumulator state (no I/O). Owned by PidController and reset on worker
// start/restart (FEAT-0003 REQ-PROFILE-05), so no PID integral/derivative memory
// carries across a profile switch. Lives outside ChannelState, so the curve law
// keeps no PID slot.
struct PidState {
    double integral = 0.0;
    double prev_temp_c = std::numeric_limits<double>::quiet_NaN();
    bool has_prev = false;
};

struct PidGains {
    double kp = 0.0;
    double ki = 0.0;
    double kd = 0.0;
};

// One PID step decomposed, exposed for unit tests and for the kind-aware
// reporting fields (REQ-PROFILE-08).
struct PidTerms {
    double error_c = std::numeric_limits<double>::quiet_NaN();
    double p_term = 0.0;
    double i_term = 0.0;
    double d_term = 0.0;
    // bias + p + i + d, before the min_duty floor / [0,100] clamp / slew cap.
    double raw_setpoint_pct = std::numeric_limits<double>::quiet_NaN();
};

// One pure PID step. error = observed_temp_c - target_c (cooling convention: a
// hotter-than-target channel drives MORE duty, so all gains are non-negative).
// Derivative-on-measurement with a POSITIVE sign -- d = kd*(temp - prev_temp)/dt
// -- so a rising temperature raises duty (anticipatory cooling) and a live
// target_c change produces no derivative kick. There is no integral or
// derivative term on the first step (no prior sample) or when dt_s <= 0.
// Anti-windup: the integral accumulator is clamped to [integral_min,
// integral_max] (NaN bound = no clamp) AND conditional integration freezes it
// whenever the raw output saturates against [output_min_pct, output_max_pct] in
// the error's direction. The returned raw_setpoint_pct is NOT itself clamped --
// the caller applies the min_duty floor, the [0,100] clamp, and the slew cap.
PidTerms PidStep(const PidGains& gains, double bias_pct,
                 double observed_temp_c, double target_c, double dt_s,
                 double output_min_pct, double output_max_pct,
                 double integral_min, double integral_max, PidState& state);

// Decides whether a PID channel may write live, per decision D6: pid.allow_live
// is set AND the channel has a positive non-NaN slew cap (max_setpoint_step_pct
// or both rise/fall rates) AND the named characterization artifact exists on
// disk. Returns true when live and clears `reason`; returns false and fills
// `reason` with the downgrade cause otherwise. Exposed for the startup downgrade
// event and for tests.
bool PidLiveAuthorized(const ChannelControlConfig& config, std::string& reason);

// PID control law (FEAT-0003 REQ-PROFILE-02). Covers P/PI/PD/PID by gain
// selection (a zero gain disables that term). Shadow/dry-run unless constructed
// live (PidLiveAuthorized): a non-live controller sets ChannelEvaluation::
// write_suppressed so the write path computes + logs but never actuates.
class PidController final : public IChannelController {
  public:
    explicit PidController(const ChannelControlConfig& config);

    ChannelEvaluation Evaluate(
        ChannelState& channel, const ControlLoopConfig& loop,
        const TempInputs& temp_inputs,
        const RuntimeSnapshotIndex& runtime_index,
        std::chrono::steady_clock::time_point now) override;
    void Reset() override;
    std::string_view Kind() const override;
    ControllerStartupInfo StartupInfo() const override;

    bool live() const { return live_; }
    const std::string& shadow_reason() const { return shadow_reason_; }

  private:
    ChannelControlConfig config_;
    PidState state_;
    bool live_ = false;
    std::string shadow_reason_;
};

}  // namespace svg_mb_control
