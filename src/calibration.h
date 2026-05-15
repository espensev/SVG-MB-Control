#pragma once

#include "control_config.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace svg_mb_control {

struct CalibrationStepSpec {
    double duty_pct = 0.0;
    std::uint32_t hold_ms = 0u;
};

struct CalibrationOptions {
    std::vector<CalibrationStepSpec> sequence;
    std::uint32_t settle_window_ms = 10000u;
    double abort_temp_ceiling_c = 95.0;
    std::optional<std::uint32_t> only_channel;
    std::filesystem::path output_path;
};

CalibrationOptions DefaultCalibrationOptions();

int RunCalibration(const ControlConfig& base,
                   const std::filesystem::path& runtime_home,
                   const CalibrationOptions& options,
                   const std::atomic<bool>& stop_flag);

}  // namespace svg_mb_control
