#include "runtime_status.h"

#include "json_io.h"
#include "runtime_event_log.h"
#include "runtime_paths.h"
#include "runtime_util.h"

#include "windows_lean.h"

#include <array>
#include <cmath>
#include <cctype>
#include <mutex>
#include <string>
#include <unordered_map>

namespace svg_mb_control {

namespace {

double JsonNumberOrZero(double value) {
    return std::isnan(value) ? 0.0 : value;
}

struct StatusPublishRegistry {
    std::mutex mutex;
    std::unordered_map<std::string, bool> active_failures;
};

StatusPublishRegistry& RuntimeStatusPublishRegistry() {
    static StatusPublishRegistry registry;
    return registry;
}

std::string NormalizeStatusPublishKey(const std::filesystem::path& path) {
    std::string key = path.lexically_normal().string();
    for (char& ch : key) {
        ch = static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
    }
    return key;
}

void ReportStatusPublishResult(const std::filesystem::path& runtime_home,
                               const std::string& mode,
                               const std::string& event_log_path,
                               bool success,
                               const std::string& write_error) {
    const std::filesystem::path status_path = RuntimeStatusPath(runtime_home);
    const std::string key = NormalizeStatusPublishKey(status_path);
    bool should_report_failure = false;
    bool should_report_recovery = false;
    {
        StatusPublishRegistry& registry = RuntimeStatusPublishRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        bool& active_failure = registry.active_failures[key];
        if (!success && !active_failure) {
            active_failure = true;
            should_report_failure = true;
        } else if (success && active_failure) {
            active_failure = false;
            should_report_recovery = true;
        }
    }

    if (!should_report_failure && !should_report_recovery) {
        return;
    }

    std::string detail = "runtime status publish ";
    detail += should_report_failure ? "failed" : "recovered";
    detail += " path=" + status_path.string();
    if (should_report_failure && !write_error.empty()) {
        detail += " detail=" + write_error;
    }

    AppendRuntimeEvent(
        runtime_home,
        RuntimeLogEvent{
            .mode = mode.empty() ? std::string("runtime") : mode,
            .event_type = should_report_failure
                ? "runtime_logging.status_publish_failed"
                : "runtime_logging.status_publish_recovered",
            .detail = detail,
            .success = should_report_recovery,
            .event_log_path = event_log_path,
        });
}

nlohmann::json ChannelStatusToJson(const ChannelState& channel) {
    // FEAT-0003 (REQ-PROFILE-08): curve-only telemetry (feedforward / smoothing,
    // the boost overlays, and low-band) is not meaningful for a PID channel -- the
    // PID law never updates those ChannelState fields, so they sit at 0.0/NaN.
    // Publish them as JSON null for a PID channel so a curve-only quantity is never
    // reported as if it were a real value (the CSV already blanks them).
    const bool is_pid = channel.controller_kind == "pid";
    const auto curve_only = [is_pid](nlohmann::json value) -> nlohmann::json {
        return is_pid ? nlohmann::json(nullptr) : std::move(value);
    };
    nlohmann::json status = {
        {"channel", channel.config.channel},
        {"total_writes", channel.total_writes},
        {"last_setpoint_pct", JsonNumberOrZero(channel.last_setpoint_pct)},
        {"last_raw_demand_pct",
         curve_only(JsonNumberOrZero(channel.last_raw_demand_pct))},
        {"last_smoothed_demand_pct",
         curve_only(JsonNumberOrZero(channel.smoothed_demand_pct))},
    };
    // Per-stage boost overlays. Keys built from kBoostStageSpecs so the
    // "last_<name>_boost_pct" contract stays driven by the table rather
    // than by four hand-rolled fields. The key strings never change, so
    // they are built once on first use (thread-safe static init) instead
    // of on every status write.
    static const std::array<std::string, kBoostStageCount> kBoostKeys =
        [] {
            std::array<std::string, kBoostStageCount> keys;
            for (std::size_t i = 0; i < kBoostStageCount; ++i) {
                keys[i] = "last_";
                keys[i].append(kBoostStageSpecs[i].name);
                keys[i].append("_boost_pct");
            }
            return keys;
        }();
    for (std::size_t i = 0; i < kBoostStageCount; ++i) {
        status[kBoostKeys[i]] = curve_only(channel.boosts[i].boost_pct);
    }
    status["last_low_band_stage_boost_pct"] =
        curve_only(channel.low_band_stage_boost_pct);
    status["last_low_band_effective_boost_pct"] =
        curve_only(channel.low_band_effective_boost_pct);
    status["last_low_band_debt"] = curve_only(channel.low_band_debt_snapshot);
    status["last_low_band_signal"] = curve_only(channel.low_band_signal_snapshot);
    status["last_low_band_cpu_scale"] =
        curve_only(channel.low_band_cpu_scale_snapshot);
    status["last_low_band_gpu_scale"] =
        curve_only(channel.low_band_gpu_scale_snapshot);
    status["low_band_stage_active"] = curve_only(channel.low_band_stage_active);
    status["low_band_eligible_ms"] = curve_only(channel.low_band_eligible_ms);
    status["low_band_activation_count"] =
        curve_only(channel.low_band_activation_count);
    status["last_response_source"] = channel.last_response_source;
    status["last_primary_temp_source"] = channel.last_primary_temp_source;
    status["last_write_reason"] = channel.last_write_reason;
    status["last_observed_temp_c"] =
        JsonNumberOrZero(channel.last_observed_temp_c);
    status["sensor_failed"] = channel.sensor_failed;
    status["consecutive_sensor_failures"] = channel.consecutive_sensor_failures;
    status["circuit_breaker_open"] = channel.circuit_breaker_open;
    status["consecutive_write_failures"] = channel.consecutive_write_failures;
    status["consecutive_sidecar_persist_failures"] =
        channel.consecutive_sidecar_persist_failures;
    status["baseline_captured"] = channel.baseline_captured;
    // FEAT-0003 (REQ-PROFILE-08): additive control-law attribution + kind-aware
    // PID evidence. PID fields are JSON null for the curve law so curve-only
    // values are never published as if they were meaningful PID numbers. Additive
    // only -- the control_loop status schema version is unchanged (consistent
    // with how FEAT-0023 added active_profile_*).
    status["controller_kind"] = channel.controller_kind;
    if (channel.controller_kind == "pid") {
        status["pid_error_c"] = JsonNumberOrZero(channel.pid_error_c);
        status["pid_p_term"] = JsonNumberOrZero(channel.pid_p_term);
        status["pid_i_term"] = JsonNumberOrZero(channel.pid_i_term);
        status["pid_d_term"] = JsonNumberOrZero(channel.pid_d_term);
        status["pid_setpoint_raw_pct"] =
            JsonNumberOrZero(channel.pid_setpoint_raw_pct);
    } else {
        status["pid_error_c"] = nullptr;
        status["pid_p_term"] = nullptr;
        status["pid_i_term"] = nullptr;
        status["pid_d_term"] = nullptr;
        status["pid_setpoint_raw_pct"] = nullptr;
    }
    return status;
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
            channel.consecutive_write_failures > 0u ||
            channel.consecutive_sidecar_persist_failures > 0u) {
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
    snapshot.hardware_access.read_state = ParseHardwareAccessState(
        JsonStringOr(*payload, "hwaccess_read_state"));
    snapshot.hardware_access.write_state = ParseHardwareAccessState(
        JsonStringOr(*payload, "hwaccess_write_state"));
    snapshot.hardware_access.read_detail =
        JsonStringOr(*payload, "hwaccess_read_detail");
    snapshot.hardware_access.write_detail =
        JsonStringOr(*payload, "hwaccess_write_detail");
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
            entry.consecutive_sidecar_persist_failures =
                JsonUInt32Or(channel, "consecutive_sidecar_persist_failures");
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
                            const std::string& last_successful_restore_iso,
                            const std::string& active_profile_name,
                            const std::string& active_profile_source,
                            const HardwareAccessStatus& hardware_access) {
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
    // FEAT-0004 (REQ-HWHEALTH-01..03/06): additive tri-state PawnIO-backed
    // read/write availability. Unknown stays explicit so absence of a
    // successful open is never reported as healthy.
    payload["hwaccess_state"] =
        HardwareAccessStateName(HardwareAccessOverallState(hardware_access));
    payload["hwaccess_read_state"] =
        HardwareAccessStateName(hardware_access.read_state);
    payload["hwaccess_write_state"] =
        HardwareAccessStateName(hardware_access.write_state);
    payload["hwaccess_read_detail"] = hardware_access.read_detail;
    payload["hwaccess_write_detail"] = hardware_access.write_detail;
    // FEAT-0023 (REQ-MPROFILE-09): observational active-profile identity.
    payload["active_profile_name"] = active_profile_name;
    payload["active_profile_source"] = active_profile_source;

    payload["controlled_channels"] = nlohmann::json::array();
    for (const auto& channel : channels) {
        payload["controlled_channels"].push_back(ChannelStatusToJson(channel));
    }

    std::string write_error;
    const bool success = TryWriteJsonFileAtomic(
        RuntimeStatusPath(runtime_home), payload, 2, &write_error);
    ReportStatusPublishResult(runtime_home,
                              mode_label,
                              event_log_path,
                              success,
                              write_error);
    return success;
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
    payload["hwaccess_state"] =
        HardwareAccessStateName(
            HardwareAccessOverallState(status.hardware_access));
    payload["hwaccess_read_state"] =
        HardwareAccessStateName(status.hardware_access.read_state);
    payload["hwaccess_write_state"] =
        HardwareAccessStateName(status.hardware_access.write_state);
    payload["hwaccess_read_detail"] = status.hardware_access.read_detail;
    payload["hwaccess_write_detail"] = status.hardware_access.write_detail;
    // FEAT-0023 (REQ-MPROFILE-09): observational active-profile identity.
    payload["active_profile_name"] = status.active_profile_name;
    payload["active_profile_source"] = status.active_profile_source;
    std::string write_error;
    const bool success = TryWriteJsonFileAtomic(
        RuntimeStatusPath(runtime_home), payload, 2, &write_error);
    ReportStatusPublishResult(runtime_home,
                              "read-loop",
                              status.event_log_path,
                              success,
                              write_error);
    return success;
}

}  // namespace svg_mb_control
