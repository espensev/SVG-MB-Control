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
    std::optional<double> loop_achieved_interval_ms;
    std::optional<double> loop_work_duration_ms;
    std::optional<double> loop_slip_ms;
    std::optional<std::int64_t> loop_overrun;
    std::optional<double> process_cpu_pct;
    std::optional<std::int64_t> process_working_set_bytes;
    std::optional<std::int64_t> process_private_bytes;
};

struct BandPercentiles {
    int n = 0;
    std::optional<double> cpu_tctl_p50, cpu_tctl_p90, cpu_tctl_max;
    std::optional<double> gpu_memjn_p50, gpu_memjn_p90, gpu_memjn_max;
    std::optional<double> gpu_env_p50, gpu_env_p90, gpu_env_max;
};

// Percentile set for a single metric (nearest-rank p50/p90/p95/p99/max + mean).
struct PercentileSet {
    int n = 0;
    std::optional<double> p50, p90, p95, p99, max, avg;
};

// Loop-timing and process-resource percentiles over all ticks of a run.
struct TimingResourceStats {
    PercentileSet loop_achieved_interval_ms;
    PercentileSet loop_work_duration_ms;
    PercentileSet loop_slip_ms;
    PercentileSet process_cpu_pct;
    PercentileSet process_working_set_bytes;
    PercentileSet process_private_bytes;
    int overrun_count = 0;
};

struct GpuChannelAtPeak {
    int channel = 0;
    std::optional<double> setpoint_at_peak_pct;
    std::optional<double> setpoint_during_load_p90;
    std::optional<double> setpoint_during_load_max;
    int load_row_count = 0;
};

// GPU-envelope peak and (when a threshold is supplied) threshold-crossing
// summary for native analyze report output.
struct GpuResponseSummary {
    bool has_peak = false;
    std::int64_t peak_row_number = 0;  // 1-based ordinal among ticks
    double peak_elapsed_s = 0.0;
    std::optional<double> peak_value_c;
    std::optional<double> threshold_c;
    std::optional<std::int64_t> above_threshold_rows;
    std::optional<double> above_threshold_seconds;
    std::optional<double> time_to_threshold_s;
    std::vector<GpuChannelAtPeak> channels;
};

struct ChannelStats {
    std::vector<double> setpoint_pct;
    std::vector<double> duty_pct;
    std::vector<double> rpm;
    double max_thermal_pressure_boost_pct = 0.0;
    double max_midband_pressure_boost_pct = 0.0;
    double max_gpu_airflow_boost_pct = 0.0;
    double max_cpu_low_soak_boost_pct = 0.0;
    // Max over rows of (sum of the 4 stage boosts present in the row +
    // low_band_effective-or-stage). This preserves the former raw-CSV
    // analyzer rule after analysis moved fully native.
    double response_boost_total = 0.0;
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
    GpuResponseSummary gpu_response;
    TimingResourceStats timing_resources;
    int authority_reasserted = 0;
    int write_failures = 0;
    int restore_failures = 0;
    std::map<std::string, int> event_severity_counts;
    std::map<std::string, int> event_error_code_counts;
};

std::optional<double> Percentile(std::vector<double> values, double pct);
std::optional<double> Median(std::vector<double> values);
std::optional<double> Mean(const std::vector<double>& values);
BandPercentiles SummariseBand(const std::vector<TickRow>& ticks, Band band);

}  // namespace svg_mb_control::analyze::report_detail
