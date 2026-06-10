#pragma once

// Pure math for the proposed CPU package-power anticipation boost stage
// (docs/cpu-power-feedforward-plan-2026-06-10.md §4). DESIGN SUPPORT ONLY:
// nothing in the runtime calls this header yet. The v1 energy decision
// (docs/cpu-work-energy-acquisition-decision-2026-06-07.md §Scope) bans
// control use of the energy telemetry, and live wiring stays gated behind
// the plan's Gates 0-3 (quarantine exit, default-on decision, lead-time
// characterization with an explicit no-go, new FEAT intake).
//
// Mirrors the unified boost integrator semantics of
// src/control/boost_stage.cpp / CONTROL_PIPELINE_MATH.md §6.1 (guard clause,
// clamp, smootherstep band scale, degenerate-band fallback to full strength)
// with one deliberate difference: an UNAVAILABLE input DECAYS the boost
// toward zero instead of holding it, so on any energy fault the
// temperature-keyed law rules alone. Unit tests:
// tests/cpp/power_anticipation_tests.cpp (core-linked for SmoothStep).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "control_math.h"

namespace svg_mb_control {
namespace power_anticipation {

// Derived package watts from one energy window:
// uJ / ms = mW, so W = delta_uj / window_ms / 1000.
// NaN (no value, never a false zero) when the window is missing or
// non-positive, or the delta is missing or negative. A zero delta is a valid
// 0 W reading (distinct from a blanked window, which arrives as NaN).
inline double PackageWattsFromWindow(double delta_uj, double window_ms) {
    if (!(window_ms > 0.0) || std::isnan(delta_uj) || delta_uj < 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return delta_uj / window_ms / 1000.0;
}

// Optional exponential smoothing across windows (the Gate 2 noise decision).
// `alpha` is the new-sample weight in (0, 1]; an out-of-range alpha applies
// no smoothing. A NaN sample resets to NaN (fail-safe: the stage decays, see
// UpdatePowerAnticipation); a NaN prev seeds from the sample.
inline double EmaUpdate(double prev, double sample, double alpha) {
    if (std::isnan(sample)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (std::isnan(prev) || !(alpha > 0.0) || alpha > 1.0) {
        return sample;
    }
    return alpha * sample + (1.0 - alpha) * prev;
}

// Holds the most recent per-window watts across the control ticks that mirror
// one resource window (~1 s window vs 250 ms tick), and ages the value out
// when the producer stalls (sample_id stops advancing), so a stalled energy
// path cannot pin the boost.
struct WattsInputState {
    std::uint64_t last_sample_id = 0u;
    std::uint32_t ticks_held = 0u;
    double watts = std::numeric_limits<double>::quiet_NaN();
};

// Per-tick input refresh. Returns the watts the stage should see this tick:
//   - sample_id == 0: no completed window yet -> NaN.
//   - new sample_id: recompute watts from this window (NaN if the window was
//     blanked), reset the hold counter.
//   - unchanged sample_id: hold the last watts for at most max_hold_ticks
//     ticks, then NaN (stale -> the stage decays).
inline double UpdateWattsInput(WattsInputState* state,
                               std::uint64_t sample_id,
                               double delta_uj,
                               double window_ms,
                               std::uint32_t max_hold_ticks) {
    if (sample_id == 0u) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (sample_id != state->last_sample_id) {
        state->last_sample_id = sample_id;
        state->ticks_held = 0u;
        state->watts = PackageWattsFromWindow(delta_uj, window_ms);
        return state->watts;
    }
    ++state->ticks_held;
    if (state->ticks_held > max_hold_ticks) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return state->watts;
}

// Band and rates in watts / %-per-second. All five parameters are required;
// any missing or invalid parameter disables the stage (returns 0), matching
// the UpdateBoostStage guard-clause semantics. Band numbers are a Gate 2 /
// FEAT decision-record output, not defaults to invent here.
struct PowerAnticipationConfig {
    double start_w = std::numeric_limits<double>::quiet_NaN();
    double full_w = std::numeric_limits<double>::quiet_NaN();
    double rise_pct_per_sec = std::numeric_limits<double>::quiet_NaN();
    double fall_pct_per_sec = std::numeric_limits<double>::quiet_NaN();
    double max_boost_pct = std::numeric_limits<double>::quiet_NaN();
};

// One integrator step. Differences from UpdateBoostStage, both deliberate:
//   - input is watts, not a temperature;
//   - NaN input DECAYS (BelowStart holds on an undefined input) -- on any
//     energy fault, blank, or stall the boost releases and the
//     temperature-keyed response rules alone.
inline double UpdatePowerAnticipation(const PowerAnticipationConfig& cfg,
                                      double current_boost_pct,
                                      std::uint64_t elapsed_ms,
                                      double package_watts) {
    if (std::isnan(cfg.start_w) || std::isnan(cfg.full_w) ||
        std::isnan(cfg.rise_pct_per_sec) || std::isnan(cfg.fall_pct_per_sec) ||
        std::isnan(cfg.max_boost_pct) ||
        cfg.rise_pct_per_sec <= 0.0 ||
        cfg.fall_pct_per_sec < 0.0 ||
        cfg.max_boost_pct <= 0.0) {
        return 0.0;
    }
    double boost = std::isnan(current_boost_pct)
        ? 0.0
        : std::clamp(current_boost_pct, 0.0, cfg.max_boost_pct);
    if (elapsed_ms == 0u) {
        return boost;
    }
    const double dt = static_cast<double>(elapsed_ms) / 1000.0;
    if (std::isnan(package_watts) || package_watts < cfg.start_w) {
        boost -= cfg.fall_pct_per_sec * dt;
    } else {
        // Degenerate band collapses to full strength at/above start_w,
        // matching the boost_stage.cpp PressureScale fallback.
        const double scale = (cfg.full_w > cfg.start_w)
            ? SmoothStep((package_watts - cfg.start_w) /
                         (cfg.full_w - cfg.start_w))
            : 1.0;
        boost += cfg.rise_pct_per_sec * scale * dt;
    }
    return std::clamp(boost, 0.0, cfg.max_boost_pct);
}

}  // namespace power_anticipation
}  // namespace svg_mb_control
