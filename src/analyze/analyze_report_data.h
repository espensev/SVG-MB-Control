#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace svg_mb_control::analyze::report_detail {

enum class Band { kIdle, kLoad, kCooldown };

struct TickRow {
    std::int64_t tick = 0;
    std::string wall_clock;
    double elapsed_s = 0.0;
    Band band = Band::kIdle;
    std::optional<double> cpu_tctl_c;
    std::optional<double> gpu_memjn_c;
    std::optional<double> gpu_envelope_c;
};

struct BandPercentiles {
    int n = 0;
    std::optional<double> cpu_tctl_p50, cpu_tctl_p90, cpu_tctl_max;
    std::optional<double> gpu_memjn_p50, gpu_memjn_p90, gpu_memjn_max;
    std::optional<double> gpu_env_p50, gpu_env_p90, gpu_env_max;
};

struct ChannelStats {
    std::vector<double> setpoint_pct;
    std::vector<double> duty_pct;
    std::vector<double> rpm;
    double max_thermal_pressure_boost_pct = 0.0;
    double max_midband_pressure_boost_pct = 0.0;
    double max_gpu_airflow_boost_pct = 0.0;
    double max_cpu_low_soak_boost_pct = 0.0;
    std::map<std::string, int> primary_source_counts;
    std::map<std::string, int> response_source_counts;
    std::map<std::string, int> write_reason_counts;
    std::optional<std::int64_t> total_writes_min;
    std::optional<std::int64_t> total_writes_max;
    int reversals = 0;
    int mode_leave_ticks = 0;
    std::optional<double> last_setpoint;
    int last_direction = 0;  // -1, 0, +1
};

struct RuntimeManifestEvidence {
    std::filesystem::path config_path;
    std::string config_sha256;
    std::filesystem::path runtime_policy_path;
    std::string runtime_policy_sha256;
    std::filesystem::path events_path;
};

// Aggregated state assembled before output. Both emitters consume this; the
// main function populates it from the SQLite queries.
struct ReportData {
    std::int64_t run_id = 0;
    std::string session_start;
    std::string mode;
    std::string status;
    std::filesystem::path manifest_path;
    std::filesystem::path csv_archive_path;
    std::optional<std::string> tool_version;
    std::optional<std::string> git_hash;
    std::optional<std::int64_t> row_count_declared;
    std::optional<std::int64_t> row_count_ingested;
    std::optional<std::int64_t> event_count_declared;
    std::optional<std::int64_t> event_count_ingested;
    std::optional<std::string> csv_flush_policy;
    std::optional<std::string> mirror_mode;
    std::optional<std::string> last_update;
    RuntimeManifestEvidence manifest_evidence;
    std::vector<TickRow> ticks;
    BandPercentiles idle;
    BandPercentiles load;
    BandPercentiles cool;
    std::map<int, ChannelStats> channels;
    std::optional<std::int64_t> onset_tick;
    std::optional<std::int64_t> response_tick;
    std::optional<double> response_delay_s;
    int authority_reasserted = 0;
    int write_failures = 0;
    int restore_failures = 0;
    std::map<std::string, int> event_severity_counts;
    std::map<std::string, int> event_error_code_counts;
};

std::optional<double> Percentile(std::vector<double> values, double pct);
std::optional<double> Median(std::vector<double> values);
BandPercentiles SummariseBand(const std::vector<TickRow>& ticks, Band band);

}  // namespace svg_mb_control::analyze::report_detail
