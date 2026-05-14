#pragma once

#include "control_loop.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <vector>

namespace svg_mb_control {

struct ChannelState {
    ChannelControlConfig config;
    bool baseline_captured = false;
    std::uint8_t baseline_duty_raw = 0u;
    std::uint8_t baseline_mode_raw = 0u;
    double last_issued_pct = std::numeric_limits<double>::quiet_NaN();
    std::chrono::steady_clock::time_point last_write_time =
        std::chrono::steady_clock::time_point{};
    bool write_active = false;
    std::chrono::steady_clock::time_point hold_deadline =
        std::chrono::steady_clock::time_point{};
    std::uint64_t total_writes = 0u;
    double last_observed_temp_c = std::numeric_limits<double>::quiet_NaN();
    double last_setpoint_pct = std::numeric_limits<double>::quiet_NaN();
    double last_raw_demand_pct = std::numeric_limits<double>::quiet_NaN();
    double smoothed_demand_pct = std::numeric_limits<double>::quiet_NaN();
    double thermal_pressure_boost_pct = 0.0;
    std::chrono::steady_clock::time_point last_evaluation_time =
        std::chrono::steady_clock::time_point{};

    std::uint32_t consecutive_write_failures = 0u;
    bool circuit_breaker_open = false;
    static constexpr std::uint32_t kMaxConsecutiveFailures = 5u;

    std::uint32_t consecutive_sensor_failures = 0u;
    bool sensor_failed = false;
    static constexpr std::uint32_t kMaxConsecutiveSensorFailures = 3u;
    static constexpr double kSafeModeFanDuty = 100.0;
};

struct ControlRuntimeContext {
    ControlRuntimeContext(ControlConfig base_config,
                          ControlLoopConfig loop_config,
                          std::filesystem::path runtime_home_path);

    ControlConfig base;
    ControlLoopConfig loop;
    std::filesystem::path runtime_home;
    RuntimeWritePolicy runtime_policy;
    std::vector<ChannelState> channels;
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
};

}  // namespace svg_mb_control
