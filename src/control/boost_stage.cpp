#include "boost_stage.h"

#include <algorithm>
#include <cmath>

namespace svg_mb_control {

namespace {

struct ResolvedInput {
    double temp_c = std::numeric_limits<double>::quiet_NaN();
    bool available = false;
};

ResolvedInput ResolveBoostInput(BoostInput input,
                                const TempInputs& temp_inputs,
                                double observed_temp_c) {
    ResolvedInput out;
    switch (input) {
        case BoostInput::ObservedTemp:
            out.temp_c = observed_temp_c;
            out.available = !std::isnan(observed_temp_c);
            return out;
        case BoostInput::GpuEnvelope:
            out.temp_c = temp_inputs.gpu_c;
            out.available =
                temp_inputs.gpu_available && !std::isnan(temp_inputs.gpu_c);
            return out;
        case BoostInput::CpuTemp:
            out.temp_c = temp_inputs.cpu_c;
            out.available =
                temp_inputs.cpu_available && !std::isnan(temp_inputs.cpu_c);
            return out;
    }
    return out;
}

double PressureScale(double temp_c, double start_c, double full_c) {
    // Degenerate window collapses to "full strength once at/above start_c",
    // matching the legacy UpdatePressureBoost fallback.
    if (!(full_c > start_c)) {
        return 1.0;
    }
    const double t = std::clamp((temp_c - start_c) / (full_c - start_c),
                                0.0, 1.0);
    return t * t * t * ((6.0 * t - 15.0) * t + 10.0);
}

}  // namespace

double UpdateBoostStage(const BoostStageSpec& spec,
                        const BoostStageConfig& cfg,
                        double current_boost_pct,
                        std::uint64_t elapsed_ms,
                        const TempInputs& temp_inputs,
                        double observed_temp_c) {
    if (std::isnan(cfg.start_c) || std::isnan(cfg.full_c) ||
        std::isnan(cfg.rise_per_unit) || std::isnan(cfg.fall_per_unit) ||
        std::isnan(cfg.max_boost_pct) ||
        cfg.rise_per_unit <= 0.0 ||
        cfg.fall_per_unit < 0.0 ||
        cfg.max_boost_pct <= 0.0) {
        return 0.0;
    }
    if (spec.release_mode == BoostReleaseMode::ExplicitRelease &&
        std::isnan(cfg.release_c)) {
        return 0.0;
    }

    double boost = std::isnan(current_boost_pct)
        ? 0.0
        : std::clamp(current_boost_pct, 0.0, cfg.max_boost_pct);
    if (elapsed_ms == 0u) {
        return boost;
    }

    const ResolvedInput input =
        ResolveBoostInput(spec.input, temp_inputs, observed_temp_c);

    const double dt = static_cast<double>(elapsed_ms) /
        (spec.rate_unit == BoostRateUnit::PerSec ? 1000.0 : 60000.0);

    enum class Zone { Rise, Hold, Decay };
    Zone zone = Zone::Hold;
    if (spec.release_mode == BoostReleaseMode::BelowStart) {
        if (!input.available) {
            return boost;
        }
        zone = (input.temp_c >= cfg.start_c) ? Zone::Rise : Zone::Decay;
    } else {  // ExplicitRelease
        if (!input.available) {
            zone = Zone::Decay;
        } else if (input.temp_c >= cfg.start_c) {
            zone = Zone::Rise;
        } else if (input.temp_c <= cfg.release_c) {
            zone = Zone::Decay;
        } else {
            zone = Zone::Hold;
        }
    }

    if (zone == Zone::Rise) {
        const double scale = PressureScale(input.temp_c, cfg.start_c,
                                           cfg.full_c);
        boost += cfg.rise_per_unit * scale * dt;
    } else if (zone == Zone::Decay) {
        boost -= cfg.fall_per_unit * dt;
    }
    return std::clamp(boost, 0.0, cfg.max_boost_pct);
}

}  // namespace svg_mb_control
