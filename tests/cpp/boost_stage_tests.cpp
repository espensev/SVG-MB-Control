// Equivalence tests for the new UpdateBoostStage helper. The "legacy"
// reference implementations below are copied verbatim from the four
// per-boost helpers in src/control/channel_evaluator.cpp at the time this
// test was authored. The test asserts that UpdateBoostStage produces the
// same trajectory as the legacy helper it replaces, across all four
// stage specs and a sequence of representative inputs that exercises
// rise, hold, decay, dead-band, max-clamp, and unavailable-input zones.

#include "boost_stage.h"

#include "test_helpers.h"

#include "control_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

// ---------------- Legacy reference (verbatim copies) -----------------

// Verbatim copy of UpdatePressureBoost from
// src/control/channel_evaluator.cpp. Used as the golden reference for
// thermal_pressure, midband_pressure, and gpu_airflow stages.
double LegacyPressureBoost(double temp_c,
                           double current_boost_pct,
                           std::uint64_t elapsed_ms,
                           double start_c,
                           double full_c,
                           double rise_pct_per_sec,
                           double fall_pct_per_sec,
                           double max_boost_pct) {
    if (std::isnan(start_c) || std::isnan(full_c) ||
        std::isnan(rise_pct_per_sec) || std::isnan(fall_pct_per_sec) ||
        std::isnan(max_boost_pct) ||
        rise_pct_per_sec <= 0.0 ||
        fall_pct_per_sec < 0.0 ||
        max_boost_pct <= 0.0) {
        return 0.0;
    }

    double boost = std::isnan(current_boost_pct)
        ? 0.0
        : std::clamp(current_boost_pct, 0.0, max_boost_pct);
    if (elapsed_ms == 0u || std::isnan(temp_c)) {
        return boost;
    }

    const double dt_seconds = static_cast<double>(elapsed_ms) / 1000.0;
    if (temp_c >= start_c) {
        double pressure_scale = 1.0;
        if (full_c > start_c) {
            double t = std::clamp(
                (temp_c - start_c) / (full_c - start_c), 0.0, 1.0);
            pressure_scale = t * t * t * ((6.0 * t - 15.0) * t + 10.0);
        }
        if (boost < max_boost_pct) {
            boost += rise_pct_per_sec * pressure_scale * dt_seconds;
        }
    } else {
        boost -= fall_pct_per_sec * dt_seconds;
    }
    return std::clamp(boost, 0.0, max_boost_pct);
}

// Verbatim copy of UpdateCpuLowSoakBoost from
// src/control/channel_evaluator.cpp, parameterised over the fields rather
// than the ChannelControlConfig struct (the math is unchanged).
double LegacyCpuLowSoakBoost(double cpu_temp_c,
                             bool cpu_available,
                             double current_boost_pct,
                             std::uint64_t elapsed_ms,
                             double start_c,
                             double full_c,
                             double release_c,
                             double rise_pct_per_min,
                             double fall_pct_per_min,
                             double max_boost_pct) {
    if (std::isnan(start_c) || std::isnan(full_c) ||
        std::isnan(release_c) || std::isnan(rise_pct_per_min) ||
        std::isnan(fall_pct_per_min) || std::isnan(max_boost_pct) ||
        rise_pct_per_min <= 0.0 ||
        fall_pct_per_min < 0.0 ||
        max_boost_pct <= 0.0) {
        return 0.0;
    }

    double boost = std::isnan(current_boost_pct)
        ? 0.0
        : std::clamp(current_boost_pct, 0.0, max_boost_pct);
    if (elapsed_ms == 0u) {
        return boost;
    }

    const double dt_minutes = static_cast<double>(elapsed_ms) / 60000.0;
    if (cpu_available && !std::isnan(cpu_temp_c)) {
        if (cpu_temp_c >= start_c) {
            const double t = std::clamp(
                (cpu_temp_c - start_c) / (full_c - start_c), 0.0, 1.0);
            const double soak_scale =
                t * t * t * ((6.0 * t - 15.0) * t + 10.0);
            boost += rise_pct_per_min * soak_scale * dt_minutes;
        } else if (cpu_temp_c <= release_c) {
            boost -= fall_pct_per_min * dt_minutes;
        }
    } else {
        boost -= fall_pct_per_min * dt_minutes;
    }
    return std::clamp(boost, 0.0, max_boost_pct);
}

// ---------------- Equivalence drivers -----------------

constexpr double kFpEpsilon = 1e-12;

struct TempStep {
    double observed_temp_c = std::numeric_limits<double>::quiet_NaN();
    double cpu_c = 0.0;
    bool cpu_available = false;
    double gpu_c = 0.0;
    bool gpu_available = false;
    std::uint64_t elapsed_ms = 250u;
};

svg_mb_control::TempInputs ToTempInputs(const TempStep& s) {
    svg_mb_control::TempInputs t;
    t.cpu_c = s.cpu_c;
    t.cpu_available = s.cpu_available;
    t.gpu_c = s.gpu_c;
    t.gpu_available = s.gpu_available;
    return t;
}

// Runs a sequence of TempSteps through both the legacy pressure-boost
// helper and UpdateBoostStage for a BelowStart spec; asserts every
// intermediate value matches. `pick_temp` selects which input the
// legacy helper sees (observed/gpu) — UpdateBoostStage selects via spec.
template <typename PickTemp>
void RunPressureEquivalence(const std::string& label,
                            const svg_mb_control::BoostStageSpec& spec,
                            const svg_mb_control::BoostStageConfig& cfg,
                            const std::vector<TempStep>& steps,
                            PickTemp pick_temp) {
    double legacy = 0.0;
    svg_mb_control::BoostStageState state;
    int tick = 0;
    for (const auto& step : steps) {
        const svg_mb_control::TempInputs inputs = ToTempInputs(step);
        legacy = LegacyPressureBoost(
            pick_temp(step), legacy, step.elapsed_ms,
            cfg.start_c, cfg.full_c,
            cfg.rise_per_unit, cfg.fall_per_unit,
            cfg.max_boost_pct);
        state.boost_pct = svg_mb_control::UpdateBoostStage(
            spec, cfg, state.boost_pct, step.elapsed_ms,
            inputs, step.observed_temp_c);
        ExpectNear(state.boost_pct, legacy, kFpEpsilon,
                   label + " tick " + std::to_string(tick));
        ++tick;
    }
}

void TestThermalPressureMatchesLegacy() {
    svg_mb_control::BoostStageConfig cfg;
    cfg.start_c = 84.0;
    cfg.full_c = 95.0;
    cfg.rise_per_unit = 0.04;   // %/sec
    cfg.fall_per_unit = 0.02;   // %/sec
    cfg.max_boost_pct = 2.0;

    std::vector<TempStep> steps;
    // Below start: legacy decays from 0 (stays 0); new should match.
    for (int i = 0; i < 4; ++i) {
        steps.push_back({80.0, 0.0, false, 0.0, false, 250u});
    }
    // Rise zone partway through window.
    for (int i = 0; i < 20; ++i) {
        steps.push_back({88.0, 0.0, false, 0.0, false, 1000u});
    }
    // Above full: full-strength rise.
    for (int i = 0; i < 20; ++i) {
        steps.push_back({99.0, 0.0, false, 0.0, false, 1000u});
    }
    // Hold at max.
    for (int i = 0; i < 5; ++i) {
        steps.push_back({99.0, 0.0, false, 0.0, false, 1000u});
    }
    // Drop below start: decay.
    for (int i = 0; i < 30; ++i) {
        steps.push_back({75.0, 0.0, false, 0.0, false, 1000u});
    }
    // Unavailable input (NaN observed) — BelowStart spec should hold.
    for (int i = 0; i < 4; ++i) {
        TempStep s;
        s.elapsed_ms = 250u;
        steps.push_back(s);  // observed_temp_c stays NaN
    }
    // Recover, rise again.
    for (int i = 0; i < 10; ++i) {
        steps.push_back({90.0, 0.0, false, 0.0, false, 1000u});
    }

    RunPressureEquivalence(
        "thermal_pressure", svg_mb_control::kBoostStageSpecs[
            static_cast<std::size_t>(svg_mb_control::BoostStage::ThermalPressure)],
        cfg, steps,
        [](const TempStep& s) { return s.observed_temp_c; });
}

void TestMidbandPressureMatchesLegacy() {
    svg_mb_control::BoostStageConfig cfg;
    cfg.start_c = 64.0;
    cfg.full_c = 82.0;
    cfg.rise_per_unit = 0.03;
    cfg.fall_per_unit = 0.015;
    cfg.max_boost_pct = 1.5;

    std::vector<TempStep> steps;
    for (int i = 0; i < 10; ++i) {
        steps.push_back({60.0, 0.0, false, 0.0, false, 1000u});
    }
    for (int i = 0; i < 25; ++i) {
        steps.push_back({70.0, 0.0, false, 0.0, false, 1000u});
    }
    for (int i = 0; i < 25; ++i) {
        steps.push_back({80.0, 0.0, false, 0.0, false, 1000u});
    }
    for (int i = 0; i < 30; ++i) {
        steps.push_back({62.0, 0.0, false, 0.0, false, 1000u});
    }
    // NaN observed — hold.
    for (int i = 0; i < 4; ++i) {
        TempStep s;
        s.elapsed_ms = 250u;
        steps.push_back(s);
    }

    RunPressureEquivalence(
        "midband_pressure", svg_mb_control::kBoostStageSpecs[
            static_cast<std::size_t>(svg_mb_control::BoostStage::MidbandPressure)],
        cfg, steps,
        [](const TempStep& s) { return s.observed_temp_c; });
}

void TestGpuAirflowMatchesLegacy() {
    svg_mb_control::BoostStageConfig cfg;
    cfg.start_c = 58.0;
    cfg.full_c = 72.0;
    cfg.rise_per_unit = 0.02;
    cfg.fall_per_unit = 0.01;
    cfg.max_boost_pct = 1.0;

    std::vector<TempStep> steps;
    // Below start (GPU available).
    for (int i = 0; i < 5; ++i) {
        steps.push_back({0.0, 0.0, false, 55.0, true, 1000u});
    }
    // Above start, mid-window.
    for (int i = 0; i < 30; ++i) {
        steps.push_back({0.0, 0.0, false, 65.0, true, 1000u});
    }
    // Above full.
    for (int i = 0; i < 20; ++i) {
        steps.push_back({0.0, 0.0, false, 78.0, true, 1000u});
    }
    // GPU unavailable mid-trajectory — BelowStart spec should hold.
    for (int i = 0; i < 6; ++i) {
        steps.push_back({0.0, 0.0, false, 0.0, false, 1000u});
    }
    // Available, below start: decay.
    for (int i = 0; i < 40; ++i) {
        steps.push_back({0.0, 0.0, false, 50.0, true, 1000u});
    }

    RunPressureEquivalence(
        "gpu_airflow", svg_mb_control::kBoostStageSpecs[
            static_cast<std::size_t>(svg_mb_control::BoostStage::GpuAirflow)],
        cfg, steps,
        [](const TempStep& s) {
            return s.gpu_available
                ? s.gpu_c
                : std::numeric_limits<double>::quiet_NaN();
        });
}

void TestCpuLowSoakMatchesLegacy() {
    svg_mb_control::BoostStageConfig cfg;
    cfg.start_c = 72.0;
    cfg.full_c = 82.0;
    cfg.release_c = 68.0;
    cfg.rise_per_unit = 0.3;   // %/min
    cfg.fall_per_unit = 0.3;   // %/min
    cfg.max_boost_pct = 0.3;

    const auto& spec = svg_mb_control::kBoostStageSpecs[
        static_cast<std::size_t>(svg_mb_control::BoostStage::CpuLowSoak)];

    std::vector<TempStep> steps;
    // Hold zone (release < cpu < start): both should stay at 0.
    for (int i = 0; i < 5; ++i) {
        steps.push_back({0.0, 70.0, true, 0.0, false, 1000u});
    }
    // Rise (cpu >= start).
    for (int i = 0; i < 60; ++i) {
        steps.push_back({0.0, 78.0, true, 0.0, false, 1000u});
    }
    // Above full: full-rate rise (then clamp).
    for (int i = 0; i < 30; ++i) {
        steps.push_back({0.0, 90.0, true, 0.0, false, 1000u});
    }
    // Hold zone (between release and start): no change.
    for (int i = 0; i < 30; ++i) {
        steps.push_back({0.0, 70.0, true, 0.0, false, 1000u});
    }
    // Decay zone (cpu <= release).
    for (int i = 0; i < 30; ++i) {
        steps.push_back({0.0, 65.0, true, 0.0, false, 1000u});
    }
    // CPU unavailable — ExplicitRelease should decay.
    for (int i = 0; i < 10; ++i) {
        steps.push_back({0.0, 0.0, false, 0.0, false, 1000u});
    }
    // Recover above start.
    for (int i = 0; i < 30; ++i) {
        steps.push_back({0.0, 80.0, true, 0.0, false, 1000u});
    }

    double legacy = 0.0;
    svg_mb_control::BoostStageState state;
    int tick = 0;
    for (const auto& step : steps) {
        const svg_mb_control::TempInputs inputs = ToTempInputs(step);
        legacy = LegacyCpuLowSoakBoost(
            step.cpu_c, step.cpu_available, legacy, step.elapsed_ms,
            cfg.start_c, cfg.full_c, cfg.release_c,
            cfg.rise_per_unit, cfg.fall_per_unit, cfg.max_boost_pct);
        state.boost_pct = svg_mb_control::UpdateBoostStage(
            spec, cfg, state.boost_pct, step.elapsed_ms,
            inputs, std::numeric_limits<double>::quiet_NaN());
        ExpectNear(state.boost_pct, legacy, kFpEpsilon,
                   std::string("cpu_low_soak tick ") + std::to_string(tick));
        ++tick;
    }
}

void TestDisabledConfigReturnsZero() {
    svg_mb_control::TempInputs inputs;
    inputs.cpu_c = 80.0;
    inputs.cpu_available = true;
    inputs.gpu_c = 75.0;
    inputs.gpu_available = true;

    // Default-constructed config has all NaN → all four specs inert.
    svg_mb_control::BoostStageConfig disabled;
    for (std::size_t i = 0; i < svg_mb_control::kBoostStageCount; ++i) {
        const auto& spec = svg_mb_control::kBoostStageSpecs[i];
        const double out = svg_mb_control::UpdateBoostStage(
            spec, disabled, /*current=*/5.0, /*elapsed=*/1000u,
            inputs, /*observed=*/80.0);
        ExpectNear(out, 0.0, kFpEpsilon,
                   std::string("disabled config returns 0 for ") +
                       std::string(spec.name));
    }

    // Non-positive rise → inert.
    svg_mb_control::BoostStageConfig bad_rise;
    bad_rise.start_c = 60.0;
    bad_rise.full_c = 80.0;
    bad_rise.release_c = 55.0;
    bad_rise.rise_per_unit = 0.0;
    bad_rise.fall_per_unit = 0.1;
    bad_rise.max_boost_pct = 1.0;
    for (std::size_t i = 0; i < svg_mb_control::kBoostStageCount; ++i) {
        const auto& spec = svg_mb_control::kBoostStageSpecs[i];
        const double out = svg_mb_control::UpdateBoostStage(
            spec, bad_rise, 5.0, 1000u, inputs, 80.0);
        ExpectNear(out, 0.0, kFpEpsilon,
                   std::string("zero-rise config returns 0 for ") +
                       std::string(spec.name));
    }

    // ExplicitRelease with NaN release_c → inert (CpuLowSoak only path).
    svg_mb_control::BoostStageConfig missing_release;
    missing_release.start_c = 72.0;
    missing_release.full_c = 82.0;
    // release_c stays NaN.
    missing_release.rise_per_unit = 0.3;
    missing_release.fall_per_unit = 0.3;
    missing_release.max_boost_pct = 0.3;
    const auto& soak_spec = svg_mb_control::kBoostStageSpecs[
        static_cast<std::size_t>(svg_mb_control::BoostStage::CpuLowSoak)];
    const double soak_out = svg_mb_control::UpdateBoostStage(
        soak_spec, missing_release, 0.1, 1000u, inputs, 80.0);
    ExpectNear(soak_out, 0.0, kFpEpsilon,
               "cpu_low_soak with NaN release_c is inert");
}

void TestElapsedZeroIsNoop() {
    svg_mb_control::BoostStageConfig cfg;
    cfg.start_c = 84.0;
    cfg.full_c = 95.0;
    cfg.rise_per_unit = 0.04;
    cfg.fall_per_unit = 0.02;
    cfg.max_boost_pct = 2.0;

    svg_mb_control::TempInputs inputs;
    const double out = svg_mb_control::UpdateBoostStage(
        svg_mb_control::kBoostStageSpecs[static_cast<std::size_t>(
            svg_mb_control::BoostStage::ThermalPressure)],
        cfg, /*current=*/0.7, /*elapsed=*/0u, inputs, /*observed=*/90.0);
    ExpectNear(out, 0.7, kFpEpsilon, "elapsed_ms=0 returns current_boost");
}

void TestNanCurrentInitialisesToZero() {
    svg_mb_control::BoostStageConfig cfg;
    cfg.start_c = 84.0;
    cfg.full_c = 95.0;
    cfg.rise_per_unit = 0.04;
    cfg.fall_per_unit = 0.02;
    cfg.max_boost_pct = 2.0;

    svg_mb_control::TempInputs inputs;
    // Below start, NaN current → legacy returns 0.0 (clamped from -decay).
    const double out = svg_mb_control::UpdateBoostStage(
        svg_mb_control::kBoostStageSpecs[static_cast<std::size_t>(
            svg_mb_control::BoostStage::ThermalPressure)],
        cfg,
        std::numeric_limits<double>::quiet_NaN(),
        1000u, inputs, /*observed=*/80.0);
    ExpectNear(out, 0.0, kFpEpsilon, "NaN current with below-start temp -> 0");
}

}  // namespace

int main() {
    TestThermalPressureMatchesLegacy();
    TestMidbandPressureMatchesLegacy();
    TestGpuAirflowMatchesLegacy();
    TestCpuLowSoakMatchesLegacy();
    TestDisabledConfigReturnsZero();
    TestElapsedZeroIsNoop();
    TestNanCurrentInitialisesToZero();
    if (g_failures > 0) {
        std::cerr << g_failures << " boost_stage equivalence failure(s)\n";
        return 1;
    }
    std::cout << "boost_stage equivalence tests OK\n";
    return 0;
}
