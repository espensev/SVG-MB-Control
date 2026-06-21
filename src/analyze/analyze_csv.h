#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace svg_mb_control::analyze {

struct ParsedFanSample {
    std::uint32_t fan_index = 0u;
    std::optional<std::int64_t> present;
    std::optional<std::string> label;
    std::optional<std::int64_t> rpm;
    std::optional<std::int64_t> tach_raw;
    std::optional<std::int64_t> tach_valid;
    std::optional<std::int64_t> duty_raw;
    std::optional<double> duty_pct;
    std::optional<std::int64_t> mode_raw;
    std::optional<std::int64_t> manual_override;
    std::optional<std::int64_t> write_allowed;
    std::optional<std::int64_t> policy_blocked;
    std::optional<std::int64_t> effective_write_allowed;
};

struct ParsedChannelSample {
    std::uint32_t channel = 0u;
    std::optional<double> observed_temp_c;
    std::optional<double> setpoint_pct;
    std::optional<double> thermal_pressure_boost_pct;
    std::optional<double> midband_pressure_boost_pct;
    std::optional<double> gpu_airflow_boost_pct;
    std::optional<double> cpu_low_soak_boost_pct;
    std::optional<std::string> primary_temp_source;
    std::optional<std::string> response_source;
    std::optional<std::string> write_reason;
    std::optional<std::int64_t> total_writes;
    std::optional<std::int64_t> write_active;
    std::optional<std::int64_t> baseline_captured;
    std::optional<double> feedforward_pct;
    std::optional<double> correction_pct;
    std::optional<double> low_band_stage_boost_pct;
    std::optional<double> low_band_effective_boost_pct;
};

struct ParsedTickRow {
    std::int64_t tick_count = 0;
    std::string wall_clock;
    std::optional<std::string> mode;
    std::optional<std::string> snapshot_time;
    std::optional<std::int64_t> snapshot_age_ms;
    std::optional<std::int64_t> amd_sensor_count;
    std::optional<std::string> amd_sensor_summary;
    std::optional<double> cpu_tctl_c;
    std::optional<double> cpu_max_c;
    std::optional<std::int64_t> gpu_available;
    std::optional<std::string> gpu_name;
    std::optional<std::string> gpu_last_warning;
    std::optional<double> gpu_core_c;
    std::optional<double> gpu_memjn_c;
    std::optional<double> gpu_hotspot_c;
    std::optional<double> gpu_envelope_c;
    std::optional<std::int64_t> fan_count;
    std::optional<std::int64_t> policy_writes_enabled_present;
    std::optional<std::int64_t> policy_writes_enabled;
    std::optional<std::string> loop_started_wall_clock;
    std::optional<std::string> loop_finished_wall_clock;
    std::optional<double> loop_work_duration_ms;
    std::optional<std::int64_t> loop_intended_interval_ms;
    std::optional<double> loop_achieved_interval_ms;
    std::optional<double> loop_slip_ms;
    std::optional<std::int64_t> loop_overrun;
    std::optional<double> process_cpu_delta_ms;
    std::optional<double> process_cpu_pct;
    std::optional<std::int64_t> process_working_set_bytes;
    std::optional<std::int64_t> process_private_bytes;
    std::optional<double> cadence_transient;
    // FEAT-0006 read-only RAPL package-energy evidence (nullable; blank in old
    // archives and whenever the logger reports disabled/unavailable).
    std::optional<std::int64_t> cpu_power_sample_id;
    std::optional<double> cpu_power_window_ms;
    std::optional<double> cpu_pkg_energy_delta_uj;
    std::optional<std::string> cpu_pkg_energy_acquisition;
    // FEAT-0006 read-only APERF/MPERF cycle evidence (nullable; blank in old
    // archives, on the baseline read, and on guard-blanked windows).
    std::optional<std::int64_t> cpu_cycles_sample_id;
    std::optional<double> cpu_cycles_window_ms;
    std::optional<double> cpu_aperf_delta;
    std::optional<double> cpu_mperf_delta;
    std::optional<std::string> cpu_cycles_acquisition;
    // FEAT-0006 all-core effective-frequency evidence (nullable; blank in old
    // archives, on the worker's baseline sweep, and on guard-blanked sweeps).
    // The off-thread package sweep carries its OWN sample id / window, separate
    // from the per-core cpu_cycles_* columns above.
    std::optional<double> cpu_aperf_delta_allcore;
    std::optional<double> cpu_mperf_delta_allcore;
    std::optional<double> cpu_cycles_window_ms_allcore;
    std::optional<std::int64_t> cpu_cycles_allcore_sample_id;
    std::optional<std::int64_t> cpu_cycles_allcore_cores;
    // FEAT-0020 read-only GPU board-power evidence (nullable; blank in old
    // archives and when there is no live nonzero NVML read). gpu_power_mw is
    // instantaneous board milliwatts (not an energy counter), summarized as
    // mean/percentile by the report.
    std::optional<std::int64_t> gpu_power_sample_id;
    std::optional<double> gpu_power_time_ms;
    std::optional<double> gpu_power_mw;
    std::optional<std::string> gpu_power_source;
    std::optional<std::string> gpu_power_acquisition;
    // FEAT-0021 read-only GPU workload-context evidence (nullable; blank in
    // old archives and when no cached context sample is available).
    std::optional<std::int64_t> gpu_context_sample_id;
    std::optional<double> gpu_context_time_ms;
    std::optional<double> gpu_context_sample_age_ms;
    std::optional<std::string> gpu_context_acquisition;
    std::optional<std::int64_t> gpu_util_gpu_pct;
    std::optional<std::int64_t> gpu_util_mem_pct;
    std::optional<std::int64_t> gpu_pstate;
    std::optional<std::int64_t> gpu_clock_graphics_mhz;
    std::optional<std::int64_t> gpu_clock_memory_mhz;
    std::optional<std::int64_t> gpu_vram_used_mb;
    std::optional<std::int64_t> gpu_vram_total_mb;
    std::vector<ParsedFanSample> fans;
    std::vector<ParsedChannelSample> channels;
};

struct CsvHeader {
    std::vector<std::string> columns;
    std::unordered_map<std::string, std::size_t> column_index;
};

// Splits a single CSV line per RFC 4180-ish rules: fields may be wrapped in
// double quotes, internal "" escapes a literal quote. Embedded commas inside
// quoted fields are preserved.
std::vector<std::string> ParseCsvLine(const std::string& line);

// Parses the header of a control-loop CSV (the first non-comment line).
CsvHeader ParseCsvHeader(const std::string& header_line);

// Parses one data row using the precomputed header. Returns nullopt if the row
// is empty or malformed.
std::optional<ParsedTickRow> ParseTickRow(const CsvHeader& header,
                                          const std::string& line);

// Top-level: opens the CSV file at `path`, skips '#' comment lines, parses
// header + every data row. Throws std::runtime_error on I/O or schema errors.
// Returns the header and rows; row vector may be empty for an empty CSV.
struct ParsedCsv {
    CsvHeader header;
    std::vector<ParsedTickRow> rows;
    // Parsed "# key=value" comment-prologue fields (schema/mode/session_start/
    // build_version/git_hash/...). Empty for CSVs without a prologue.
    std::unordered_map<std::string, std::string> prologue;
};

ParsedCsv ParseControlLoopCsv(const std::filesystem::path& path);

}  // namespace svg_mb_control::analyze
