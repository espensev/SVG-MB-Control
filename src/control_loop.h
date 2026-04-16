#pragma once

#include "control_config.h"
#include "control_policy.h"
#include "runtime_write_policy.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace svg_mb_control {

struct ChannelControlConfig {
    std::uint32_t channel = 0u;
    TempBlend temp_blend = TempBlend::CpuOnly;
    double min_duty_pct = 0.0;
    std::uint32_t write_cooldown_ms = 0u;
    double deadband_pct = std::numeric_limits<double>::quiet_NaN();
    std::uint32_t control_hold_ms = 0u;
    std::vector<CurvePoint> curve;
};

struct ControlLoopConfig {
    std::uint32_t poll_tick_ms = 2000u;
    std::uint32_t write_cooldown_ms = 10000u;
    double deadband_pct = 3.0;
    std::uint32_t control_hold_ms = 60000u;
    std::string cpu_temp_label = "Tctl/Tdie";
    std::vector<ChannelControlConfig> channels;
};

// Loads the `control_loop` subtree from a JSON config file that also
// carries the base ControlConfig fields. Throws std::runtime_error on
// malformed or missing required fields. The returned config's paths
// (none at this layer) are left raw; the caller resolves them against
// the base ControlConfig.
ControlLoopConfig LoadControlLoopConfig(
    const std::filesystem::path& config_path);

class ControlLoop {
  public:
    ControlLoop(ControlConfig base_config,
                ControlLoopConfig loop_config,
                std::filesystem::path runtime_home);
    ~ControlLoop();

    ControlLoop(const ControlLoop&) = delete;
    ControlLoop& operator=(const ControlLoop&) = delete;

    // Runs until stop_flag is signaled. Returns 0 on clean shutdown,
    // non-zero if runtime-policy setup or the direct writer cannot be
    // started, or if a shutdown restore fails.
    int RunUntilStopped(const std::atomic<bool>& stop_flag);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace svg_mb_control
