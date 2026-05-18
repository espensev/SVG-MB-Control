#include "control_status_writer.h"

#include "json_io.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cmath>

namespace svg_mb_control {

namespace {

double JsonNumberOrZero(double value) {
    return std::isnan(value) ? 0.0 : value;
}

nlohmann::json ChannelStatusToJson(const ChannelState& channel) {
    return {
        {"channel", channel.config.channel},
        {"total_writes", channel.total_writes},
        {"last_setpoint_pct", JsonNumberOrZero(channel.last_setpoint_pct)},
        {"last_raw_demand_pct", JsonNumberOrZero(channel.last_raw_demand_pct)},
        {"last_smoothed_demand_pct",
         JsonNumberOrZero(channel.smoothed_demand_pct)},
        {"last_thermal_pressure_boost_pct",
         channel.thermal_pressure_boost_pct},
        {"last_cpu_low_soak_boost_pct", channel.cpu_low_soak_boost_pct},
        {"last_low_band_stage_boost_pct",
         channel.low_band_stage_boost_pct},
        {"last_low_band_debt", channel.low_band_debt_snapshot},
        {"last_low_band_signal", channel.low_band_signal_snapshot},
        {"last_low_band_cpu_scale", channel.low_band_cpu_scale_snapshot},
        {"last_low_band_gpu_scale", channel.low_band_gpu_scale_snapshot},
        {"low_band_stage_active", channel.low_band_stage_active},
        {"low_band_eligible_ms", channel.low_band_eligible_ms},
        {"low_band_activation_count", channel.low_band_activation_count},
        {"last_response_source", channel.last_response_source},
        {"last_write_reason", channel.last_write_reason},
        {"last_observed_temp_c", JsonNumberOrZero(channel.last_observed_temp_c)},
        {"sensor_failed", channel.sensor_failed},
        {"consecutive_sensor_failures",
         channel.consecutive_sensor_failures},
        {"circuit_breaker_open", channel.circuit_breaker_open},
        {"consecutive_write_failures",
         channel.consecutive_write_failures},
        {"baseline_captured", channel.baseline_captured},
    };
}

}  // namespace

bool WriteControlLoopStatus(const std::filesystem::path& runtime_home,
                            const std::string& mode_label,
                            const std::string& status,
                            const std::string& status_detail,
                            std::uint64_t tick_count,
                            const std::string& last_evaluation_iso,
                            const RuntimeControlLoopTimingState& timing,
                            const std::vector<ChannelState>& channels,
                            const std::string& log_csv_path,
                            const std::string& log_manifest_path,
                            const std::string& event_log_path,
                            const std::string& last_successful_restore_iso) {
    nlohmann::json payload = MakeSchemaObject(4u);
    payload["mode"] = mode_label;
    payload["process_id"] = GetCurrentProcessId();
    payload["status"] = status;
    payload["status_detail"] = status_detail;
    payload["loop_tick_count"] = tick_count;
    payload["loop_last_evaluation"] = last_evaluation_iso;
    payload["loop_started_wall_clock"] = timing.loop_started_wall_clock;
    payload["loop_finished_wall_clock"] = timing.loop_finished_wall_clock;
    payload["loop_work_duration_ms"] =
        JsonNumberOrZero(timing.loop_work_duration_ms);
    payload["loop_intended_interval_ms"] =
        timing.loop_intended_interval_ms;
    payload["loop_achieved_interval_ms"] =
        JsonNumberOrZero(timing.loop_achieved_interval_ms);
    payload["loop_slip_ms"] = JsonNumberOrZero(timing.loop_slip_ms);
    payload["loop_overrun"] = timing.loop_overrun;
    payload["process_cpu_delta_ms"] =
        JsonNumberOrZero(timing.process_cpu_delta_ms);
    payload["process_cpu_pct"] = JsonNumberOrZero(timing.process_cpu_pct);
    payload["process_working_set_bytes"] =
        timing.process_working_set_bytes;
    payload["process_private_bytes"] = timing.process_private_bytes;
    payload["log_csv_path"] = log_csv_path;
    payload["log_manifest_path"] = log_manifest_path;
    payload["event_log_path"] = event_log_path;
    payload["last_successful_restore_time"] = last_successful_restore_iso;

    payload["controlled_channels"] = nlohmann::json::array();
    for (const auto& channel : channels) {
        payload["controlled_channels"].push_back(ChannelStatusToJson(channel));
    }

    return TryWriteJsonFileAtomic(runtime_home / "control_runtime.json",
                                  payload);
}

std::vector<RuntimeControlChannelLogState> BuildChannelLogStates(
    const std::vector<ChannelState>& channels) {
    std::vector<RuntimeControlChannelLogState> log_states;
    log_states.reserve(channels.size());
    for (const auto& channel : channels) {
        RuntimeControlChannelLogState state;
        state.channel = channel.config.channel;
        state.observed_temp_c = channel.last_observed_temp_c;
        state.setpoint_pct = channel.last_setpoint_pct;
        state.feedforward_pct = channel.last_raw_demand_pct;
        state.thermal_pressure_boost_pct =
            channel.thermal_pressure_boost_pct;
        state.cpu_low_soak_boost_pct = channel.cpu_low_soak_boost_pct;
        state.low_band_stage_boost_pct =
            channel.low_band_stage_boost_pct;
        state.low_band_debt = channel.low_band_debt_snapshot;
        state.low_band_signal = channel.low_band_signal_snapshot;
        state.low_band_stage_active = channel.low_band_stage_active;
        state.response_source = channel.last_response_source;
        state.write_reason = channel.last_write_reason;
        state.total_writes = channel.total_writes;
        state.write_active = channel.write_active;
        state.baseline_captured = channel.baseline_captured;
        log_states.push_back(state);
    }
    return log_states;
}

}  // namespace svg_mb_control
