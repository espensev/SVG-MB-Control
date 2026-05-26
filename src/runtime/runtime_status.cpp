#include "runtime_status.h"

#include "json_io.h"
#include "runtime_paths.h"
#include "runtime_util.h"

#include "windows_lean.h"

#include <cmath>
#include <utility>

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
        {"last_midband_pressure_boost_pct",
         channel.midband_pressure_boost_pct},
        {"last_gpu_airflow_boost_pct", channel.gpu_airflow_boost_pct},
        {"last_cpu_low_soak_boost_pct", channel.cpu_low_soak_boost_pct},
        {"last_low_band_stage_boost_pct",
         channel.low_band_stage_boost_pct},
        {"last_low_band_effective_boost_pct",
         channel.low_band_effective_boost_pct},
        {"last_low_band_debt", channel.low_band_debt_snapshot},
        {"last_low_band_signal", channel.low_band_signal_snapshot},
        {"last_low_band_cpu_scale", channel.low_band_cpu_scale_snapshot},
        {"last_low_band_gpu_scale", channel.low_band_gpu_scale_snapshot},
        {"low_band_stage_active", channel.low_band_stage_active},
        {"low_band_eligible_ms", channel.low_band_eligible_ms},
        {"low_band_activation_count", channel.low_band_activation_count},
        {"last_response_source", channel.last_response_source},
        {"last_write_reason", channel.last_write_reason},
        {"last_observed_temp_c",
         JsonNumberOrZero(channel.last_observed_temp_c)},
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

bool RuntimeStatusSnapshot::LooksActive() const {
    if (status == "shutdown" || status == "failed" ||
        status == "direct-read-failed") {
        return false;
    }
    return process_id != 0u && IsProcessActive(process_id);
}

std::uint32_t RuntimeStatusSnapshot::DegradedChannelCount() const {
    std::uint32_t count = 0u;
    for (const auto& channel : controlled_channels) {
        if (channel.circuit_breaker_open || channel.sensor_failed ||
            channel.consecutive_write_failures > 0u) {
            ++count;
        }
    }
    return count;
}

std::optional<RuntimeStatusSnapshot> ReadRuntimeStatus(
    const std::filesystem::path& runtime_home) {
    const auto payload = TryReadJsonObject(
        RuntimeStatusPath(runtime_home), "runtime status");
    if (!payload.has_value()) {
        return std::nullopt;
    }

    RuntimeStatusSnapshot snapshot;
    snapshot.mode = JsonStringOr(*payload, "mode");
    snapshot.status = JsonStringOr(*payload, "status");
    snapshot.status_detail = JsonStringOr(*payload, "status_detail");
    snapshot.process_id = JsonUInt32Or(*payload, "process_id");
    // The two schemas use different field names for the latest update
    // timestamp. Resolve here so consumers do not branch on schema.
    snapshot.last_update_iso = JsonStringOr(
        *payload, "loop_last_evaluation",
        JsonStringOr(*payload, "last_refresh"));
    snapshot.last_successful_restore_iso =
        JsonStringOr(*payload, "last_successful_restore_time");
    snapshot.log_csv_path = JsonStringOr(*payload, "log_csv_path");
    snapshot.event_log_path = JsonStringOr(*payload, "event_log_path");
    snapshot.stale = JsonBoolOr(*payload, "stale");

    if (const auto channels = payload->find("controlled_channels");
        channels != payload->end() && channels->is_array()) {
        snapshot.controlled_channels.reserve(channels->size());
        for (const auto& channel : *channels) {
            if (!channel.is_object()) {
                continue;
            }
            RuntimeStatusChannelSnapshot entry;
            entry.channel = JsonUInt32Or(channel, "channel");
            entry.circuit_breaker_open =
                JsonBoolOr(channel, "circuit_breaker_open");
            entry.sensor_failed = JsonBoolOr(channel, "sensor_failed");
            entry.consecutive_write_failures =
                JsonUInt32Or(channel, "consecutive_write_failures");
            snapshot.controlled_channels.push_back(entry);
        }
    }

    return snapshot;
}

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
    // control_runtime.json is dual-schema: control-loop and read-loop both
    // write the same path with different field sets and different
    // schema_version numbers. The mode field is the discriminator at the
    // semantic level; the explicit schema field makes that contract
    // visible to consumers without dispatching on mode.
    nlohmann::json payload = MakeSchemaObject(4u);
    payload["schema"] = "svg_mb_control.runtime_status.control_loop.v4";
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
    payload["cadence_transient"] =
        JsonNumberOrZero(timing.cadence_transient);
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

    return TryWriteJsonFileAtomic(RuntimeStatusPath(runtime_home), payload);
}

bool WriteReadLoopStatus(const std::filesystem::path& runtime_home,
                         const ReadLoop::Status& status) {
    nlohmann::json payload = MakeSchemaObject(1u);
    payload["schema"] = "svg_mb_control.runtime_status.read_loop.v1";
    payload["status"] = status.status;
    payload["mode"] = "read-loop";
    payload["process_id"] = GetCurrentProcessId();
    payload["status_detail"] = status.status_detail;
    payload["last_refresh"] = status.last_refresh_iso;
    payload["snapshot_source"] = status.snapshot_source;
    payload["restart_count"] = status.restart_count;
    payload["skipped_polls"] = status.skipped_polls;
    payload["successful_polls"] = status.successful_polls;
    payload["stale"] = status.stale;
    payload["child_pid"] = status.child_pid;
    payload["log_csv_path"] = status.log_csv_path;
    payload["log_manifest_path"] = status.log_manifest_path;
    payload["event_log_path"] = status.event_log_path;
    return TryWriteJsonFileAtomic(RuntimeStatusPath(runtime_home), payload);
}

}  // namespace svg_mb_control
