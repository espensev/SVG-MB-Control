#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svg_mb_control::analyze {

struct ManifestData {
    std::string session_start;
    std::string mode;
    std::string status;
    std::optional<std::string> tool_version;
    std::optional<std::string> git_hash;
    std::optional<std::int64_t> row_count;
    std::optional<std::int64_t> event_count;
    std::optional<std::string> csv_flush_policy;
    std::optional<std::string> mirror_mode;
    std::optional<std::string> last_update;
    std::optional<std::filesystem::path> csv_archive_path;
};

struct EventData {
    std::string event_time;
    std::string event_type;
    std::optional<std::string> severity;
    std::optional<std::string> error_code;
    std::optional<std::string> mode;
    std::optional<std::int64_t> success;
    std::optional<std::int64_t> channel;
    std::optional<double> setpoint_pct;
    std::optional<double> observed_temp_c;
    std::optional<std::int64_t> tick_count;
    std::optional<std::string> detail;
    std::string extra_json;
};

struct PlantModelStep {
    std::int64_t step_index = 0;
    double duty_pct_target = 0.0;
    std::int64_t hold_ms = 0;
    std::int64_t settle_window_ms = 0;
    std::optional<std::int64_t> settle_sample_count;
    std::optional<double> duty_pct_observed_mean;
    std::optional<double> rpm_mean;
    std::optional<double> rpm_stddev;
    std::optional<double> tctl_c_mean;
    std::optional<double> gpu_envelope_c_mean;
};

struct PlantModelChannel {
    std::int64_t channel = 0;
    std::optional<std::int64_t> baseline_captured;
    std::optional<std::int64_t> restored;
    std::optional<std::int64_t> baseline_duty_raw;
    std::optional<std::int64_t> baseline_mode_raw;
    std::optional<double> baseline_rpm;
    std::optional<double> baseline_tctl_c;
    std::optional<double> baseline_gpu_envelope_c;
    std::optional<std::string> note;
    std::vector<PlantModelStep> steps;
};

struct PlantModelData {
    std::string captured_local;
    std::optional<std::string> abort_reason;
    std::int64_t settle_window_ms = 0;
    double abort_temp_ceiling_c = 0.0;
    std::optional<std::string> tool_version;
    std::optional<std::string> git_hash;
    std::string sequence_json;
    std::vector<PlantModelChannel> channels;
};

ManifestData ParseRuntimeManifest(const std::filesystem::path& path);
std::vector<EventData> ParseEventsJsonl(const std::filesystem::path& path);
PlantModelData ParsePlantModelCapture(const std::filesystem::path& path);

}  // namespace svg_mb_control::analyze
