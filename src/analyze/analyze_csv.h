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
    std::optional<std::string> response_source;
    std::optional<std::string> write_reason;
    std::optional<std::int64_t> total_writes;
    std::optional<std::int64_t> write_active;
    std::optional<std::int64_t> baseline_captured;
    std::optional<double> feedforward_pct;
    std::optional<double> correction_pct;
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
    std::vector<ParsedFanSample> fans;
    std::vector<ParsedChannelSample> channels;
};

struct CsvHeader {
    std::vector<std::string> columns;
    std::unordered_map<std::string, std::size_t> column_index;
};

struct ParseError {
    std::string message;
    std::size_t line_number = 0;
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
};

ParsedCsv ParseControlLoopCsv(const std::filesystem::path& path);

}  // namespace svg_mb_control::analyze
