#pragma once

#include "control_policy.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

namespace svg_mb_control {

// Stable index for each boost overlay. Reused as the array index into
// per-channel BoostStageConfig and BoostStageState. The CSV/status field
// ordering and the response_source modifier order both come from this
// enum, so do not reorder without checking the existing field contract.
enum class BoostStage : std::size_t {
    ThermalPressure = 0,
    MidbandPressure,
    GpuAirflow,
    CpuLowSoak,
    Count,
};
inline constexpr std::size_t kBoostStageCount =
    static_cast<std::size_t>(BoostStage::Count);

enum class BoostInput {
    ObservedTemp,    // The channel's primary curve temperature this tick.
    GpuEnvelope,     // temp_inputs.gpu_c (+ gpu_available).
    CpuTemp,         // temp_inputs.cpu_c (+ cpu_available).
};

enum class BoostRateUnit {
    PerSec,
    PerMin,
};

enum class BoostReleaseMode {
    // Single threshold: rise when temp_c >= start_c, decay when below.
    // When the input is unavailable, the boost holds. Matches the existing
    // UpdatePressureBoost contract used by thermal_pressure, midband_pressure,
    // and gpu_airflow.
    BelowStart,
    // Three zones: rise above start_c, decay at or below release_c, hold
    // in between. When the input is unavailable, decay. Matches the
    // existing UpdateCpuLowSoakBoost contract.
    ExplicitRelease,
};

struct BoostStageSpec {
    std::string_view name;          // "thermal_pressure", etc.
    std::string_view response_tag;  // appended into last_response_source.
    BoostInput input;
    BoostRateUnit rate_unit;
    BoostReleaseMode release_mode;
    bool is_primary;  // freezes low-band debt accrual while active.
};

inline constexpr std::array<BoostStageSpec, kBoostStageCount>
    kBoostStageSpecs{{
        {"thermal_pressure", "thermal_pressure",
         BoostInput::ObservedTemp, BoostRateUnit::PerSec,
         BoostReleaseMode::BelowStart, true},
        {"midband_pressure", "midband_pressure",
         BoostInput::ObservedTemp, BoostRateUnit::PerSec,
         BoostReleaseMode::BelowStart, true},
        {"gpu_airflow", "gpu_airflow",
         BoostInput::GpuEnvelope, BoostRateUnit::PerSec,
         BoostReleaseMode::BelowStart, true},
        {"cpu_low_soak", "cpu_low_soak",
         BoostInput::CpuTemp, BoostRateUnit::PerMin,
         BoostReleaseMode::ExplicitRelease, false},
    }};

struct BoostStageConfig {
    double start_c = std::numeric_limits<double>::quiet_NaN();
    double full_c = std::numeric_limits<double>::quiet_NaN();
    // ExplicitRelease only; ignored for BelowStart.
    double release_c = std::numeric_limits<double>::quiet_NaN();
    double rise_per_unit = std::numeric_limits<double>::quiet_NaN();
    double fall_per_unit = std::numeric_limits<double>::quiet_NaN();
    double max_boost_pct = std::numeric_limits<double>::quiet_NaN();
};

struct BoostStageState {
    double boost_pct = 0.0;
};

// Advances one boost overlay by `elapsed_ms`. Returns the new boost value
// in [0, max_boost_pct], or 0.0 when the config is incomplete/non-positive
// (matching the disabled-path contract from the legacy per-boost helpers).
//
// `observed_temp_c` is the channel's selected primary curve temperature
// for this tick; it is only consumed for specs whose input is
// BoostInput::ObservedTemp. Pass quiet_NaN when there is no primary input
// (the BelowStart short-circuit then holds the integrator).
double UpdateBoostStage(const BoostStageSpec& spec,
                        const BoostStageConfig& cfg,
                        double current_boost_pct,
                        std::uint64_t elapsed_ms,
                        const TempInputs& temp_inputs,
                        double observed_temp_c);

}  // namespace svg_mb_control
