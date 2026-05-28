#include "low_band_integrator.h"

#include "control_math.h"
#include "low_band_evidence.h"
#include "runtime_event_log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <vector>

namespace svg_mb_control {

namespace {

// Above this threshold the low-band signal is treated as meaningfully
// non-zero, gating debt accrual. Pure epsilon, not a tuning knob.
constexpr double kSignalEpsilonPct = 0.0001;

// Per-channel boost level (in % of duty) at which a primary response is
// considered "engaged" for the purposes of freezing low-band debt
// accrual. Cross-references kBoostStageSpecs[i].is_primary via
// ChannelState::HasPrimaryResponseAbove.
constexpr double kPrimaryResponseFreezeThresholdPct = 0.05;

// True when the temperature input either has no telemetry or has fallen
// to/below its release threshold. Mirrors the symmetric CPU/GPU release
// semantics used by the debt-decay branch.
bool SensorReleased(bool available, double temp_c, double release_c) {
    return !available || temp_c <= release_c;
}

// True when any channel reports a primary boost above the freeze
// threshold. The boost fields reflect the previous tick because
// UpdateLowBandState runs before per-channel evaluation; the resulting
// one-tick lag is acceptable here.
bool PrimaryResponseActive(const std::vector<ChannelState>& channels) {
    for (const auto& ch : channels) {
        if (ch.HasPrimaryResponseAbove(kPrimaryResponseFreezeThresholdPct)) {
            return true;
        }
    }
    return false;
}

// Low-band debt accrues only when (a) the smootherstep-scaled signal is
// meaningfully non-zero and (b) no primary response is already carrying
// the load.
bool ShouldAccrueDebt(double signal, bool primary_response_active) {
    return signal > kSignalEpsilonPct && !primary_response_active;
}

// Symmetric release condition: debt only decays once both CPU and GPU
// have fallen back into their respective released bands (or gone away).
bool ShouldReleaseDebt(bool cpu_released, bool gpu_released) {
    return cpu_released && gpu_released;
}

// Stage activations are rate-limited: a freshly activated channel must
// wait stage_spacing_ms past the most recent activation before another
// channel may activate. The first activation always satisfies the gate.
bool StageSpacingSatisfied(const LowBandRuntimeState& state,
                           std::chrono::milliseconds spacing,
                           std::chrono::steady_clock::time_point now) {
    return !state.have_last_stage_activation ||
           (now - state.last_stage_activation_time) >= spacing;
}

}  // namespace

void UpdateLowBandState(ControlRuntimeContext& context,
                        const TempInputs& temp_inputs,
                        const RuntimeSnapshot& runtime_snapshot,
                        std::uint64_t elapsed_ms,
                        std::chrono::steady_clock::time_point now,
                        std::uint64_t tick_count) {
    LowBandRuntimeState& state = context.low_band;
    const LowBandControlConfig& cfg = context.loop.low_band;
    state.enabled = cfg.enabled;
    if (!cfg.enabled) {
        return;
    }

    if (elapsed_ms == 0u) {
        elapsed_ms = context.loop.poll_tick_ms;
    }
    const double dt_minutes = static_cast<double>(elapsed_ms) / 60000.0;
    const double dt_seconds = static_cast<double>(elapsed_ms) / 1000.0;

    const double cpu_scale = temp_inputs.cpu_available
        ? SmoothScale(temp_inputs.cpu_c, cfg.cpu_start_c, cfg.cpu_full_c)
        : 0.0;
    const double gpu_scale = temp_inputs.gpu_available
        ? SmoothScale(temp_inputs.gpu_c, cfg.gpu_start_c, cfg.gpu_full_c)
        : 0.0;
    const double signal = std::clamp(
        (std::max)(cfg.cpu_weight * cpu_scale, cfg.gpu_weight * gpu_scale),
        0.0, 1.0);

    const bool cpu_released = SensorReleased(temp_inputs.cpu_available,
                                             temp_inputs.cpu_c,
                                             cfg.cpu_release_c);
    const bool gpu_released = SensorReleased(temp_inputs.gpu_available,
                                             temp_inputs.gpu_c,
                                             cfg.gpu_release_c);
    const bool primary_response_active = PrimaryResponseActive(context.channels);

    if (ShouldAccrueDebt(signal, primary_response_active)) {
        state.debt += cfg.rise_per_min * signal * dt_minutes;
    } else if (ShouldReleaseDebt(cpu_released, gpu_released)) {
        state.debt -= cfg.fall_per_min * dt_minutes;
    }
    state.debt = std::clamp(state.debt, 0.0, 1.0);
    state.signal = signal;
    state.cpu_scale = cpu_scale;
    state.gpu_scale = gpu_scale;
    ++state.sample_count;
    if (state.debt > 0.0001) {
        ++state.active_sample_count;
    }
    state.max_debt = (std::max)(state.max_debt, state.debt);
    if (temp_inputs.cpu_available) {
        state.max_cpu_c = std::isnan(state.max_cpu_c)
            ? temp_inputs.cpu_c
            : (std::max)(state.max_cpu_c, temp_inputs.cpu_c);
    }
    if (temp_inputs.gpu_available) {
        state.max_gpu_c = std::isnan(state.max_gpu_c)
            ? temp_inputs.gpu_c
            : (std::max)(state.max_gpu_c, temp_inputs.gpu_c);
    }

    const auto stage_spacing =
        std::chrono::milliseconds(cfg.stage_spacing_ms);

    for (auto& channel : context.channels) {
        channel.low_band_debt_snapshot = state.debt;
        channel.low_band_signal_snapshot = signal;
        channel.low_band_cpu_scale_snapshot = cpu_scale;
        channel.low_band_gpu_scale_snapshot = gpu_scale;
        ++channel.low_band_sample_count;

        const bool configured = LowBandChannelConfigured(channel);
        double target_boost_pct = 0.0;
        if (configured) {
            const double threshold = channel.config.low_band_debt_threshold;
            if (state.debt >= threshold) {
                channel.low_band_eligible_ms += elapsed_ms;
            } else {
                channel.low_band_eligible_ms = 0u;
                if (state.debt <= threshold * 0.75) {
                    channel.low_band_stage_active = false;
                }
            }

            const bool spacing_ok =
                StageSpacingSatisfied(state, stage_spacing, now);
            if (!channel.low_band_stage_active &&
                channel.low_band_eligible_ms >=
                    channel.config.low_band_hold_ms &&
                spacing_ok) {
                channel.low_band_stage_active = true;
                ++channel.low_band_activation_count;
                state.have_last_stage_activation = true;
                state.last_stage_activation_time = now;
                std::ostringstream detail;
                detail << "low-band stage activated stage="
                       << channel.config.low_band_stage
                       << " debt=" << state.debt
                       << " threshold=" << threshold
                       << " cap_pct="
                       << channel.config.low_band_max_boost_pct;
                AppendControlLoopEvent(
                    context.runtime_home,
                    RuntimeLogEvent{
                        .event_type = "control_loop.low_band_stage_activated",
                        .detail = detail.str(),
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .success = true,
                    });
            }

            if (channel.low_band_stage_active) {
                const double scale =
                    SmoothScale(state.debt, threshold, 1.0);
                target_boost_pct =
                    channel.config.low_band_max_boost_pct * scale;
            }
        } else {
            channel.low_band_stage_active = false;
            channel.low_band_eligible_ms = 0u;
        }

        channel.low_band_stage_boost_pct = std::clamp(
            MoveTowardRateLimited(
                channel.low_band_stage_boost_pct,
                target_boost_pct,
                dt_minutes,
                cfg.stage_rise_pct_per_min,
                cfg.stage_fall_pct_per_min),
            0.0,
            configured ? channel.config.low_band_max_boost_pct : 0.0);

        if (channel.low_band_stage_boost_pct > 0.0005) {
            ++channel.low_band_active_sample_count;
            channel.low_band_boost_area_pct_s +=
                channel.low_band_stage_boost_pct * dt_seconds;
            channel.low_band_max_boost_pct = (std::max)(
                channel.low_band_max_boost_pct,
                channel.low_band_stage_boost_pct);
        }

        if (const RuntimeFanSnapshot* fan = FindRuntimeFanChannel(
                runtime_snapshot, channel.config.channel)) {
            if (fan->tach_valid) {
                if (channel.low_band_stage_boost_pct > 0.0005) {
                    channel.low_band_boosted_rpm_sum +=
                        static_cast<double>(fan->rpm);
                    ++channel.low_band_boosted_rpm_count;
                } else {
                    channel.low_band_unboosted_rpm_sum +=
                        static_cast<double>(fan->rpm);
                    ++channel.low_band_unboosted_rpm_count;
                }
            }
        }
    }
}

}  // namespace svg_mb_control
