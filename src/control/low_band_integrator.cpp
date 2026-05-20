#include "low_band_integrator.h"

#include "cadence_score.h"
#include "low_band_evidence.h"
#include "runtime_event_log.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace svg_mb_control {

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

    const bool cpu_released =
        !temp_inputs.cpu_available || temp_inputs.cpu_c <= cfg.cpu_release_c;
    const bool gpu_released =
        !temp_inputs.gpu_available || temp_inputs.gpu_c <= cfg.gpu_release_c;

    // Low-band is second priority. Freeze debt accrual while any channel is
    // running a primary response (mid-band pressure, GPU airflow, or
    // high-temperature thermal pressure) so low-band does not keep building
    // debt on top of the responses that are already carrying the load. The
    // boost fields reflect the previous tick (UpdateLowBandState runs before
    // per-channel evaluation); a one-tick lag is acceptable here. Decay still
    // runs once temperatures fall back into the released band.
    bool primary_response_active = false;
    for (const auto& ch : context.channels) {
        if (ch.midband_pressure_boost_pct > 0.05 ||
            ch.gpu_airflow_boost_pct > 0.05 ||
            ch.thermal_pressure_boost_pct > 0.05) {
            primary_response_active = true;
            break;
        }
    }

    if (signal > 0.0001 && !primary_response_active) {
        state.debt += cfg.rise_per_min * signal * dt_minutes;
    } else if (cpu_released && gpu_released) {
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
                !state.have_last_stage_activation ||
                (now - state.last_stage_activation_time) >= stage_spacing;
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
                AppendRuntimeEvent(
                    context.runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
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
