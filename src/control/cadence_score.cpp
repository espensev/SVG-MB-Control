#include "cadence_score.h"

#include <algorithm>
#include <cmath>

namespace svg_mb_control {

double SmoothStep(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * t * ((6.0 * t - 15.0) * t + 10.0);
}

double SmoothScale(double value, double start, double full) {
    if (std::isnan(value) || full <= start) {
        return 0.0;
    }
    return SmoothStep((value - start) / (full - start));
}

double MoveTowardRateLimited(double current,
                             double target,
                             double dt_minutes,
                             double rise_per_min,
                             double fall_per_min) {
    current = std::isnan(current) ? 0.0 : current;
    const double delta = target - current;
    if (std::abs(delta) <= 0.0001 || dt_minutes <= 0.0) {
        return target;
    }

    const double rate = delta > 0.0 ? rise_per_min : fall_per_min;
    if (std::isnan(rate) || rate <= 0.0) {
        return target;
    }

    const double allowed = rate * dt_minutes;
    if (std::abs(delta) <= allowed) {
        return target;
    }
    return current + (delta > 0.0 ? allowed : -allowed);
}

CadenceTick ComputeCadence(const ControlLoopConfig& cfg,
                           const TempInputs& temp_inputs,
                           double dt_ms,
                           CadenceRuntimeState& state) {
    const double P = static_cast<double>(cfg.poll_tick_ms);
    const double F = static_cast<double>(cfg.poll_tick_floor_ms);

    CadenceTick out;
    if (F >= P) {
        // Adaptation disabled (default-inert when poll_tick_floor_ms is
        // absent): no transient, E == P, byte-identical to the pre-feature
        // loop.
        out.effective_interval_ms = cfg.poll_tick_ms;
        return out;
    }
    if (std::isnan(state.effective_ms)) {
        state.effective_ms = P;
    }

    const double dt_s = dt_ms / 1000.0;
    if (std::isfinite(dt_ms) && dt_s > 0.0) {
        double slew_c_per_s = 0.0;
        if (temp_inputs.cpu_available && !std::isnan(state.prev_cpu_c)) {
            slew_c_per_s = (std::max)(slew_c_per_s,
                std::abs(temp_inputs.cpu_c - state.prev_cpu_c) / dt_s);
        }
        if (temp_inputs.gpu_available && !std::isnan(state.prev_gpu_c)) {
            slew_c_per_s = (std::max)(slew_c_per_s,
                std::abs(temp_inputs.gpu_c - state.prev_gpu_c) / dt_s);
        }
        out.transient = SmoothScale(slew_c_per_s,
                                    cfg.cadence_slew_start_c_per_s,
                                    cfg.cadence_slew_full_c_per_s);
    }

    // Carry this tick's temperatures for the next slew delta. NaN when a
    // sensor is unavailable so a sampling gap cannot fabricate a slew.
    state.prev_cpu_c = temp_inputs.cpu_available
        ? temp_inputs.cpu_c : std::numeric_limits<double>::quiet_NaN();
    state.prev_gpu_c = temp_inputs.gpu_available
        ? temp_inputs.gpu_c : std::numeric_limits<double>::quiet_NaN();

    const double target = std::round(P - out.transient * (P - F));
    // Tighten (target below current) responds immediately; relax (target
    // above current, decaying back toward P) is rate-limited to
    // cadence_relax_per_s. MoveTowardRateLimited takes per-minute rates
    // with dt in minutes.
    const double dt_min = dt_s / 60.0;
    constexpr double kInstantPerMin = 1.0e12;
    state.effective_ms = MoveTowardRateLimited(
        state.effective_ms, target, dt_min,
        /*rise_per_min=*/cfg.cadence_relax_per_s * 60.0,  // up = slow relax
        /*fall_per_min=*/kInstantPerMin);                 // down = instant
    state.effective_ms = std::clamp(state.effective_ms, F, P);
    out.effective_interval_ms =
        static_cast<std::uint32_t>(std::lround(state.effective_ms));
    return out;
}

}  // namespace svg_mb_control
