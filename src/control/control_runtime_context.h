#pragma once

#include "control_loop.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace svg_mb_control {

// FEAT-0003: per-channel control law, owned by ControlRuntimeContext index-aligned
// with `channels`. Forward-declared so this header stays free of
// channel_controller.h (which includes this one); the out-of-line destructor in
// control_runtime_context.cpp instantiates the unique_ptr deleter where the type
// is complete.
class IChannelController;

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
    // Per-stage boost contribution, indexed by BoostStage. Updated by
    // UpdateBoostStage once per tick in EvaluateChannel.
    std::array<BoostStageState, kBoostStageCount> boosts{};

    // True when any spec marked is_primary in kBoostStageSpecs has a
    // current boost above the supplied threshold. Used by the low-band
    // integrator to freeze global debt accrual while a primary response
    // (mid-band pressure, GPU airflow, or thermal pressure) is active so
    // low-band does not pile on top of an already-active response.
    bool HasPrimaryResponseAbove(double threshold_pct) const {
        for (std::size_t i = 0; i < kBoostStageCount; ++i) {
            if (!kBoostStageSpecs[i].is_primary) {
                continue;
            }
            if (boosts[i].boost_pct > threshold_pct) {
                return true;
            }
        }
        return false;
    }
    double low_band_stage_boost_pct = 0.0;
    double low_band_effective_boost_pct = 0.0;
    double low_band_debt_snapshot = 0.0;
    double low_band_signal_snapshot = 0.0;
    double low_band_cpu_scale_snapshot = 0.0;
    double low_band_gpu_scale_snapshot = 0.0;
    bool low_band_stage_active = false;
    std::uint64_t low_band_eligible_ms = 0u;
    std::uint64_t low_band_activation_count = 0u;
    std::uint64_t low_band_sample_count = 0u;
    std::uint64_t low_band_active_sample_count = 0u;
    double low_band_boost_area_pct_s = 0.0;
    double low_band_max_boost_pct = 0.0;
    double low_band_unboosted_rpm_sum = 0.0;
    std::uint64_t low_band_unboosted_rpm_count = 0u;
    double low_band_boosted_rpm_sum = 0.0;
    std::uint64_t low_band_boosted_rpm_count = 0u;
    std::string last_primary_temp_source = "unavailable";
    std::string last_response_source = "unavailable";
    std::string last_write_reason = "none";

    // FEAT-0003: control-law attribution + PID evidence for reporting. The
    // controller_kind is set once at construction from the channel's
    // IChannelController::Kind(). The pid_* fields are written each tick only by
    // PidController; they stay NaN for the curve law so the CSV/status reporting
    // blanks them for curve channels (REQ-PROFILE-08).
    std::string controller_kind = "curve_overlay";
    double pid_error_c = std::numeric_limits<double>::quiet_NaN();
    double pid_p_term = std::numeric_limits<double>::quiet_NaN();
    double pid_i_term = std::numeric_limits<double>::quiet_NaN();
    double pid_d_term = std::numeric_limits<double>::quiet_NaN();
    double pid_setpoint_raw_pct = std::numeric_limits<double>::quiet_NaN();
    std::chrono::steady_clock::time_point last_evaluation_time =
        std::chrono::steady_clock::time_point{};

    std::uint32_t consecutive_write_failures = 0u;
    bool circuit_breaker_open = false;
    static constexpr std::uint32_t kMaxConsecutiveFailures = 5u;
    // FEAT-0011: time of the last half-open probe write while the write-failure
    // breaker is open. Bounds probe frequency to kBreakerProbeBackoff so a
    // recovered actuator can self-heal on rising cooling demand without spamming
    // a still-failing one every tick.
    std::chrono::steady_clock::time_point last_probe_time =
        std::chrono::steady_clock::time_point{};
    static constexpr std::chrono::milliseconds kBreakerProbeBackoff{5000};

    // FEAT-0010 (REQ-WRITESAFE-03): consecutive failures to persist the
    // pending-write sidecar (PendingWritesStore::Upsert throwing). A sidecar
    // persist failure does NOT veto the fan write and does NOT touch the
    // write-failure breaker; this counter degrades runtime health and resets to
    // 0 on the next successful persist.
    std::uint32_t consecutive_sidecar_persist_failures = 0u;

    std::uint32_t consecutive_sensor_failures = 0u;
    bool sensor_failed = false;
    // FEAT-0013: true once this channel has seen a CPU input available. Used to
    // distinguish a CPU *dropout* (CPU was available, now gone while GPU remains)
    // from a never-present GPU-led configuration on source-aware channels.
    bool cpu_input_was_available = false;
    static constexpr std::uint32_t kMaxConsecutiveSensorFailures = 3u;
    static constexpr double kSafeModeFanDuty = 100.0;
};

struct LowBandRuntimeState {
    bool enabled = false;
    double debt = 0.0;
    double signal = 0.0;
    double cpu_scale = 0.0;
    double gpu_scale = 0.0;
    std::uint64_t sample_count = 0u;
    std::uint64_t active_sample_count = 0u;
    double max_debt = 0.0;
    double max_cpu_c = std::numeric_limits<double>::quiet_NaN();
    double max_gpu_c = std::numeric_limits<double>::quiet_NaN();
    bool have_last_stage_activation = false;
    std::chrono::steady_clock::time_point last_stage_activation_time =
        std::chrono::steady_clock::time_point{};
};

struct ControlRuntimeContext {
    ControlRuntimeContext(ControlConfig base_config,
                          ControlLoopConfig loop_config,
                          std::filesystem::path runtime_home_path);
    // Defined in the .cpp where IChannelController is complete (the controllers
    // vector holds unique_ptr<IChannelController>, an incomplete type here).
    ~ControlRuntimeContext();

    ControlConfig base;
    ControlLoopConfig loop;
    std::filesystem::path runtime_home;
    RuntimeWritePolicy runtime_policy;
    std::vector<ChannelState> channels;
    // FEAT-0003: one control-law controller per entry in `channels`, built in the
    // same constructor loop so the two vectors stay the same size and order. The
    // tick loop dispatches controllers[i]->Evaluate(channels[i], ...).
    std::vector<std::unique_ptr<IChannelController>> controllers;
    LowBandRuntimeState low_band;
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
};

}  // namespace svg_mb_control
