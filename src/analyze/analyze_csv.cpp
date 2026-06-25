#include "analyze_csv.h"

#include "analyze_channel_sample_columns.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace svg_mb_control::analyze {

namespace {

bool TryParseInt64(std::string_view text, std::int64_t& out) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

bool TryParseDouble(std::string_view text, double& out) {
    if (text.empty()) {
        return false;
    }
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    if (auto [ptr, ec] = std::from_chars(begin, end, out);
        ec == std::errc{} && ptr == end) {
        return true;
    }
    try {
        std::size_t consumed = 0u;
        const std::string owned(text);
        out = std::stod(owned, &consumed);
        return consumed == owned.size();
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<std::int64_t> AsInt(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    if (text == "true") {
        return std::int64_t{1};
    }
    if (text == "false") {
        return std::int64_t{0};
    }
    std::int64_t value = 0;
    if (TryParseInt64(text, value)) {
        return value;
    }
    return std::nullopt;
}

std::optional<double> AsDouble(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    double value = 0.0;
    if (TryParseDouble(text, value)) {
        return value;
    }
    return std::nullopt;
}

std::optional<std::string> AsText(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    return text;
}

const std::string& GetFieldByIndex(const std::vector<std::string>& fields,
                                   int index) {
    static const std::string kEmpty;
    if (index < 0 || static_cast<std::size_t>(index) >= fields.size()) {
        return kEmpty;
    }
    return fields[static_cast<std::size_t>(index)];
}

bool HasColumn(const CsvHeader& header, const std::string& name) {
    return header.column_index.find(name) != header.column_index.end();
}

// Resolves a column name to its field index (TickRowParsePlan::kAbsent when the
// column is missing), so the per-name hashing happens once at plan-build time
// instead of on every row. Mirrors GetField's old absent-column handling: a
// missing name yields kAbsent, which GetFieldByIndex then reads as empty.
int ColumnIndex(const CsvHeader& header, const std::string& name) {
    auto it = header.column_index.find(name);
    if (it == header.column_index.end()) {
        return TickRowParsePlan::kAbsent;
    }
    return static_cast<int>(it->second);
}

void SetChannelSampleField(ParsedChannelSample& ch,
                           TickChannelSampleColumn id,
                           const std::string& value) {
    switch (id) {
        case TickChannelSampleColumn::ObservedTempC:
            ch.observed_temp_c = AsDouble(value);
            break;
        case TickChannelSampleColumn::SetpointPct:
            ch.setpoint_pct = AsDouble(value);
            break;
        case TickChannelSampleColumn::ThermalPressureBoostPct:
            ch.thermal_pressure_boost_pct = AsDouble(value);
            break;
        case TickChannelSampleColumn::MidbandPressureBoostPct:
            ch.midband_pressure_boost_pct = AsDouble(value);
            break;
        case TickChannelSampleColumn::GpuAirflowBoostPct:
            ch.gpu_airflow_boost_pct = AsDouble(value);
            break;
        case TickChannelSampleColumn::CpuLowSoakBoostPct:
            ch.cpu_low_soak_boost_pct = AsDouble(value);
            break;
        case TickChannelSampleColumn::PrimaryTempSource:
            ch.primary_temp_source = AsText(value);
            break;
        case TickChannelSampleColumn::ResponseSource:
            ch.response_source = AsText(value);
            break;
        case TickChannelSampleColumn::WriteReason:
            ch.write_reason = AsText(value);
            break;
        case TickChannelSampleColumn::TotalWrites:
            ch.total_writes = AsInt(value);
            break;
        case TickChannelSampleColumn::WriteActive:
            ch.write_active = AsInt(value);
            break;
        case TickChannelSampleColumn::BaselineCaptured:
            ch.baseline_captured = AsInt(value);
            break;
        case TickChannelSampleColumn::FeedforwardPct:
            ch.feedforward_pct = AsDouble(value);
            break;
        case TickChannelSampleColumn::CorrectionPct:
            ch.correction_pct = AsDouble(value);
            break;
        case TickChannelSampleColumn::LowBandStageBoostPct:
            ch.low_band_stage_boost_pct = AsDouble(value);
            break;
        case TickChannelSampleColumn::LowBandEffectiveBoostPct:
            ch.low_band_effective_boost_pct = AsDouble(value);
            break;
    }
}

std::optional<double> GpuEnvelopeC(std::optional<double> core_c,
                                   std::optional<double> memjn_c,
                                   std::optional<double> hotspot_c) {
    std::optional<double> envelope;
    const auto consider = [&envelope](std::optional<double> value) {
        if (!value.has_value()) {
            return;
        }
        if (!envelope.has_value() || *value > *envelope) {
            envelope = value;
        }
    };
    consider(core_c);
    consider(memjn_c);
    if (hotspot_c.has_value() && *hotspot_c > 0.0) {
        consider(hotspot_c);
    }
    return envelope;
}

}  // namespace

std::vector<std::string> ParseCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool in_quotes = false;
    std::size_t i = 0;
    const std::size_t n = line.size();
    while (i < n) {
        const char ch = line[i];
        if (in_quotes) {
            if (ch == '"') {
                if (i + 1 < n && line[i + 1] == '"') {
                    current.push_back('"');
                    i += 2;
                    continue;
                }
                in_quotes = false;
                ++i;
                continue;
            }
            current.push_back(ch);
            ++i;
            continue;
        }
        if (ch == '"') {
            in_quotes = true;
            ++i;
            continue;
        }
        if (ch == ',') {
            fields.push_back(std::move(current));
            current.clear();
            ++i;
            continue;
        }
        current.push_back(ch);
        ++i;
    }
    fields.push_back(std::move(current));
    return fields;
}

CsvHeader ParseCsvHeader(const std::string& header_line) {
    CsvHeader header;
    header.columns = ParseCsvLine(header_line);
    header.column_index.reserve(header.columns.size());
    for (std::size_t idx = 0; idx < header.columns.size(); ++idx) {
        header.column_index[header.columns[idx]] = idx;
    }
    return header;
}

TickRowParsePlan BuildTickRowParsePlan(const CsvHeader& header) {
    TickRowParsePlan plan;
    plan.wall_clock = ColumnIndex(header, "wall_clock");
    plan.loop_tick_count = ColumnIndex(header, "loop_tick_count");
    plan.mode = ColumnIndex(header, "mode");
    plan.snapshot_time = ColumnIndex(header, "snapshot_time");
    plan.snapshot_age_ms = ColumnIndex(header, "snapshot_age_ms");
    plan.amd_sensor_count = ColumnIndex(header, "amd_sensor_count");
    plan.amd_sensor_summary = ColumnIndex(header, "amd_sensor_summary");
    plan.cpu_tctl_c = ColumnIndex(header, "cpu_tctl_c");
    plan.cpu_max_c = ColumnIndex(header, "cpu_max_c");
    plan.gpu_available = ColumnIndex(header, "gpu_available");
    plan.gpu_name = ColumnIndex(header, "gpu_name");
    plan.gpu_last_warning = ColumnIndex(header, "gpu_last_warning");
    plan.gpu_core_c = ColumnIndex(header, "gpu_core_c");
    plan.gpu_memjn_c = ColumnIndex(header, "gpu_memjn_c");
    plan.gpu_hotspot_c = ColumnIndex(header, "gpu_hotspot_c");
    plan.gpu_envelope_c = ColumnIndex(header, "gpu_envelope_c");
    plan.fan_count = ColumnIndex(header, "fan_count");
    plan.policy_writes_enabled_present =
        ColumnIndex(header, "policy_writes_enabled_present");
    plan.policy_writes_enabled = ColumnIndex(header, "policy_writes_enabled");
    plan.loop_started_wall_clock =
        ColumnIndex(header, "loop_started_wall_clock");
    plan.loop_finished_wall_clock =
        ColumnIndex(header, "loop_finished_wall_clock");
    plan.loop_work_duration_ms = ColumnIndex(header, "loop_work_duration_ms");
    plan.loop_intended_interval_ms =
        ColumnIndex(header, "loop_intended_interval_ms");
    plan.loop_achieved_interval_ms =
        ColumnIndex(header, "loop_achieved_interval_ms");
    plan.loop_slip_ms = ColumnIndex(header, "loop_slip_ms");
    plan.loop_overrun = ColumnIndex(header, "loop_overrun");
    plan.process_cpu_delta_ms = ColumnIndex(header, "process_cpu_delta_ms");
    plan.process_cpu_pct = ColumnIndex(header, "process_cpu_pct");
    plan.process_working_set_bytes =
        ColumnIndex(header, "process_working_set_bytes");
    plan.process_private_bytes = ColumnIndex(header, "process_private_bytes");
    plan.cadence_transient = ColumnIndex(header, "cadence_transient");
    plan.cpu_power_sample_id = ColumnIndex(header, "cpu_power_sample_id");
    plan.cpu_power_window_ms = ColumnIndex(header, "cpu_power_window_ms");
    plan.cpu_pkg_energy_delta_uj =
        ColumnIndex(header, "cpu_pkg_energy_delta_uj");
    plan.cpu_pkg_energy_acquisition =
        ColumnIndex(header, "cpu_pkg_energy_acquisition");
    plan.cpu_cycles_sample_id = ColumnIndex(header, "cpu_cycles_sample_id");
    plan.cpu_cycles_window_ms = ColumnIndex(header, "cpu_cycles_window_ms");
    plan.cpu_aperf_delta = ColumnIndex(header, "cpu_aperf_delta");
    plan.cpu_mperf_delta = ColumnIndex(header, "cpu_mperf_delta");
    plan.cpu_cycles_acquisition = ColumnIndex(header, "cpu_cycles_acquisition");
    plan.cpu_aperf_delta_allcore =
        ColumnIndex(header, "cpu_aperf_delta_allcore");
    plan.cpu_mperf_delta_allcore =
        ColumnIndex(header, "cpu_mperf_delta_allcore");
    plan.cpu_cycles_window_ms_allcore =
        ColumnIndex(header, "cpu_cycles_window_ms_allcore");
    plan.cpu_cycles_allcore_sample_id =
        ColumnIndex(header, "cpu_cycles_allcore_sample_id");
    plan.cpu_cycles_allcore_cores =
        ColumnIndex(header, "cpu_cycles_allcore_cores");
    plan.gpu_power_sample_id = ColumnIndex(header, "gpu_power_sample_id");
    plan.gpu_power_time_ms = ColumnIndex(header, "gpu_power_time_ms");
    plan.gpu_power_mw = ColumnIndex(header, "gpu_power_mw");
    plan.gpu_power_source = ColumnIndex(header, "gpu_power_source");
    plan.gpu_power_acquisition = ColumnIndex(header, "gpu_power_acquisition");
    plan.gpu_context_sample_id = ColumnIndex(header, "gpu_context_sample_id");
    plan.gpu_context_time_ms = ColumnIndex(header, "gpu_context_time_ms");
    plan.gpu_context_sample_age_ms =
        ColumnIndex(header, "gpu_context_sample_age_ms");
    plan.gpu_context_acquisition =
        ColumnIndex(header, "gpu_context_acquisition");
    plan.gpu_util_gpu_pct = ColumnIndex(header, "gpu_util_gpu_pct");
    plan.gpu_util_mem_pct = ColumnIndex(header, "gpu_util_mem_pct");
    plan.gpu_pstate = ColumnIndex(header, "gpu_pstate");
    plan.gpu_clock_graphics_mhz = ColumnIndex(header, "gpu_clock_graphics_mhz");
    plan.gpu_clock_memory_mhz = ColumnIndex(header, "gpu_clock_memory_mhz");
    plan.gpu_vram_used_mb = ColumnIndex(header, "gpu_vram_used_mb");
    plan.gpu_vram_total_mb = ColumnIndex(header, "gpu_vram_total_mb");

    for (std::uint32_t fi = 0u; fi < 64u; ++fi) {
        const std::string prefix = "fan" + std::to_string(fi) + "_";
        if (!HasColumn(header, prefix + "present")) {
            break;
        }
        TickRowParsePlan::FanColumns fan_cols;
        fan_cols.fan_index = fi;
        fan_cols.present = ColumnIndex(header, prefix + "present");
        fan_cols.label = ColumnIndex(header, prefix + "label");
        fan_cols.rpm = ColumnIndex(header, prefix + "rpm");
        fan_cols.tach_raw = ColumnIndex(header, prefix + "tach_raw");
        fan_cols.tach_valid = ColumnIndex(header, prefix + "tach_valid");
        fan_cols.duty_raw = ColumnIndex(header, prefix + "duty_raw");
        fan_cols.duty_pct = ColumnIndex(header, prefix + "duty_pct");
        fan_cols.mode_raw = ColumnIndex(header, prefix + "mode_raw");
        fan_cols.manual_override =
            ColumnIndex(header, prefix + "manual_override");
        fan_cols.write_allowed = ColumnIndex(header, prefix + "write_allowed");
        fan_cols.policy_blocked = ColumnIndex(header, prefix + "policy_blocked");
        fan_cols.effective_write_allowed =
            ColumnIndex(header, prefix + "effective_write_allowed");
        plan.fans.push_back(fan_cols);
    }

    const auto& channel_specs = TickChannelSampleColumns();
    const char* const observed_temp_name = TickChannelSampleColumnById(
        TickChannelSampleColumn::ObservedTempC).name;
    for (std::uint32_t ci = 0u; ci < 64u; ++ci) {
        if (!HasColumn(header,
                       TickChannelCsvFieldName(ci, observed_temp_name))) {
            break;
        }
        TickRowParsePlan::ChannelColumns chan_cols;
        chan_cols.channel = ci;
        chan_cols.field_index.reserve(channel_specs.size());
        for (const auto& spec : channel_specs) {
            chan_cols.field_index.push_back(
                ColumnIndex(header, TickChannelCsvFieldName(ci, spec.name)));
        }
        plan.channels.push_back(std::move(chan_cols));
    }

    return plan;
}

std::optional<ParsedTickRow> ParseTickRow(const CsvHeader& header,
                                          const TickRowParsePlan& plan,
                                          const std::string& line) {
    (void)header;  // Column resolution is precomputed in `plan`.
    if (line.empty()) {
        return std::nullopt;
    }
    const std::vector<std::string> fields = ParseCsvLine(line);
    if (fields.size() < 2u) {
        return std::nullopt;
    }

    ParsedTickRow row;
    row.wall_clock = GetFieldByIndex(fields, plan.wall_clock);
    if (row.wall_clock.empty()) {
        return std::nullopt;
    }
    if (auto tick = AsInt(GetFieldByIndex(fields, plan.loop_tick_count));
        tick.has_value()) {
        row.tick_count = *tick;
    } else {
        return std::nullopt;
    }

    row.mode = AsText(GetFieldByIndex(fields, plan.mode));
    row.snapshot_time = AsText(GetFieldByIndex(fields, plan.snapshot_time));
    row.snapshot_age_ms = AsInt(GetFieldByIndex(fields, plan.snapshot_age_ms));
    row.amd_sensor_count = AsInt(GetFieldByIndex(fields, plan.amd_sensor_count));
    row.amd_sensor_summary = AsText(
        GetFieldByIndex(fields, plan.amd_sensor_summary));
    row.cpu_tctl_c = AsDouble(GetFieldByIndex(fields, plan.cpu_tctl_c));
    row.cpu_max_c = AsDouble(GetFieldByIndex(fields, plan.cpu_max_c));
    row.gpu_available = AsInt(GetFieldByIndex(fields, plan.gpu_available));
    row.gpu_name = AsText(GetFieldByIndex(fields, plan.gpu_name));
    row.gpu_last_warning = AsText(
        GetFieldByIndex(fields, plan.gpu_last_warning));
    row.gpu_core_c = AsDouble(GetFieldByIndex(fields, plan.gpu_core_c));
    row.gpu_memjn_c = AsDouble(GetFieldByIndex(fields, plan.gpu_memjn_c));
    row.gpu_hotspot_c = AsDouble(GetFieldByIndex(fields, plan.gpu_hotspot_c));
    row.gpu_envelope_c = AsDouble(GetFieldByIndex(fields, plan.gpu_envelope_c));
    if (!row.gpu_envelope_c.has_value()) {
        row.gpu_envelope_c = GpuEnvelopeC(
            row.gpu_core_c, row.gpu_memjn_c, row.gpu_hotspot_c);
    }
    row.fan_count = AsInt(GetFieldByIndex(fields, plan.fan_count));
    row.policy_writes_enabled_present = AsInt(
        GetFieldByIndex(fields, plan.policy_writes_enabled_present));
    row.policy_writes_enabled = AsInt(
        GetFieldByIndex(fields, plan.policy_writes_enabled));
    row.loop_started_wall_clock = AsText(
        GetFieldByIndex(fields, plan.loop_started_wall_clock));
    row.loop_finished_wall_clock = AsText(
        GetFieldByIndex(fields, plan.loop_finished_wall_clock));
    row.loop_work_duration_ms = AsDouble(
        GetFieldByIndex(fields, plan.loop_work_duration_ms));
    row.loop_intended_interval_ms = AsInt(
        GetFieldByIndex(fields, plan.loop_intended_interval_ms));
    row.loop_achieved_interval_ms = AsDouble(
        GetFieldByIndex(fields, plan.loop_achieved_interval_ms));
    row.loop_slip_ms = AsDouble(GetFieldByIndex(fields, plan.loop_slip_ms));
    row.loop_overrun = AsInt(GetFieldByIndex(fields, plan.loop_overrun));
    row.process_cpu_delta_ms = AsDouble(
        GetFieldByIndex(fields, plan.process_cpu_delta_ms));
    row.process_cpu_pct = AsDouble(
        GetFieldByIndex(fields, plan.process_cpu_pct));
    row.process_working_set_bytes = AsInt(
        GetFieldByIndex(fields, plan.process_working_set_bytes));
    row.process_private_bytes = AsInt(
        GetFieldByIndex(fields, plan.process_private_bytes));
    row.cadence_transient = AsDouble(
        GetFieldByIndex(fields, plan.cadence_transient));
    row.cpu_power_sample_id = AsInt(
        GetFieldByIndex(fields, plan.cpu_power_sample_id));
    row.cpu_power_window_ms = AsDouble(
        GetFieldByIndex(fields, plan.cpu_power_window_ms));
    row.cpu_pkg_energy_delta_uj = AsDouble(
        GetFieldByIndex(fields, plan.cpu_pkg_energy_delta_uj));
    row.cpu_pkg_energy_acquisition = AsText(
        GetFieldByIndex(fields, plan.cpu_pkg_energy_acquisition));
    row.cpu_cycles_sample_id = AsInt(
        GetFieldByIndex(fields, plan.cpu_cycles_sample_id));
    row.cpu_cycles_window_ms = AsDouble(
        GetFieldByIndex(fields, plan.cpu_cycles_window_ms));
    row.cpu_aperf_delta = AsDouble(
        GetFieldByIndex(fields, plan.cpu_aperf_delta));
    row.cpu_mperf_delta = AsDouble(
        GetFieldByIndex(fields, plan.cpu_mperf_delta));
    row.cpu_cycles_acquisition = AsText(
        GetFieldByIndex(fields, plan.cpu_cycles_acquisition));
    row.cpu_aperf_delta_allcore = AsDouble(
        GetFieldByIndex(fields, plan.cpu_aperf_delta_allcore));
    row.cpu_mperf_delta_allcore = AsDouble(
        GetFieldByIndex(fields, plan.cpu_mperf_delta_allcore));
    row.cpu_cycles_window_ms_allcore = AsDouble(
        GetFieldByIndex(fields, plan.cpu_cycles_window_ms_allcore));
    row.cpu_cycles_allcore_sample_id = AsInt(
        GetFieldByIndex(fields, plan.cpu_cycles_allcore_sample_id));
    row.cpu_cycles_allcore_cores = AsInt(
        GetFieldByIndex(fields, plan.cpu_cycles_allcore_cores));
    row.gpu_power_sample_id = AsInt(
        GetFieldByIndex(fields, plan.gpu_power_sample_id));
    row.gpu_power_time_ms = AsDouble(
        GetFieldByIndex(fields, plan.gpu_power_time_ms));
    row.gpu_power_mw = AsDouble(
        GetFieldByIndex(fields, plan.gpu_power_mw));
    row.gpu_power_source = AsText(
        GetFieldByIndex(fields, plan.gpu_power_source));
    row.gpu_power_acquisition = AsText(
        GetFieldByIndex(fields, plan.gpu_power_acquisition));
    row.gpu_context_sample_id = AsInt(
        GetFieldByIndex(fields, plan.gpu_context_sample_id));
    row.gpu_context_time_ms = AsDouble(
        GetFieldByIndex(fields, plan.gpu_context_time_ms));
    row.gpu_context_sample_age_ms = AsDouble(
        GetFieldByIndex(fields, plan.gpu_context_sample_age_ms));
    row.gpu_context_acquisition = AsText(
        GetFieldByIndex(fields, plan.gpu_context_acquisition));
    row.gpu_util_gpu_pct = AsInt(
        GetFieldByIndex(fields, plan.gpu_util_gpu_pct));
    row.gpu_util_mem_pct = AsInt(
        GetFieldByIndex(fields, plan.gpu_util_mem_pct));
    row.gpu_pstate = AsInt(GetFieldByIndex(fields, plan.gpu_pstate));
    row.gpu_clock_graphics_mhz = AsInt(
        GetFieldByIndex(fields, plan.gpu_clock_graphics_mhz));
    row.gpu_clock_memory_mhz = AsInt(
        GetFieldByIndex(fields, plan.gpu_clock_memory_mhz));
    row.gpu_vram_used_mb = AsInt(
        GetFieldByIndex(fields, plan.gpu_vram_used_mb));
    row.gpu_vram_total_mb = AsInt(
        GetFieldByIndex(fields, plan.gpu_vram_total_mb));

    row.fans.reserve(plan.fans.size());
    for (const auto& fan_cols : plan.fans) {
        ParsedFanSample fan;
        fan.fan_index = fan_cols.fan_index;
        fan.present = AsInt(GetFieldByIndex(fields, fan_cols.present));
        fan.label = AsText(GetFieldByIndex(fields, fan_cols.label));
        fan.rpm = AsInt(GetFieldByIndex(fields, fan_cols.rpm));
        fan.tach_raw = AsInt(GetFieldByIndex(fields, fan_cols.tach_raw));
        fan.tach_valid = AsInt(GetFieldByIndex(fields, fan_cols.tach_valid));
        fan.duty_raw = AsInt(GetFieldByIndex(fields, fan_cols.duty_raw));
        fan.duty_pct = AsDouble(GetFieldByIndex(fields, fan_cols.duty_pct));
        fan.mode_raw = AsInt(GetFieldByIndex(fields, fan_cols.mode_raw));
        fan.manual_override = AsInt(
            GetFieldByIndex(fields, fan_cols.manual_override));
        fan.write_allowed = AsInt(
            GetFieldByIndex(fields, fan_cols.write_allowed));
        fan.policy_blocked = AsInt(
            GetFieldByIndex(fields, fan_cols.policy_blocked));
        fan.effective_write_allowed = AsInt(
            GetFieldByIndex(fields, fan_cols.effective_write_allowed));
        row.fans.push_back(std::move(fan));
    }

    const auto& channel_specs = TickChannelSampleColumns();
    row.channels.reserve(plan.channels.size());
    for (const auto& chan_cols : plan.channels) {
        ParsedChannelSample ch;
        ch.channel = chan_cols.channel;
        for (std::size_t si = 0u; si < channel_specs.size(); ++si) {
            const int field_index =
                si < chan_cols.field_index.size()
                    ? chan_cols.field_index[si]
                    : TickRowParsePlan::kAbsent;
            SetChannelSampleField(ch, channel_specs[si].id,
                                  GetFieldByIndex(fields, field_index));
        }
        row.channels.push_back(std::move(ch));
    }

    return row;
}

std::optional<ParsedTickRow> ParseTickRow(const CsvHeader& header,
                                          const std::string& line) {
    const TickRowParsePlan plan = BuildTickRowParsePlan(header);
    return ParseTickRow(header, plan, line);
}

ParsedCsv ParseControlLoopCsv(const std::filesystem::path& path) {
    ParsedCsv result;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("Failed to open CSV: " + path.string());
    }

    std::string line;
    bool header_parsed = false;
    TickRowParsePlan plan;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (line[0] == '#') {
            std::string body = line.substr(1);
            if (!body.empty() && body.front() == ' ') {
                body.erase(0, 1);
            }
            const auto eq = body.find('=');
            if (eq != std::string::npos) {
                result.prologue.emplace(body.substr(0, eq),
                                        body.substr(eq + 1));
            }
            continue;
        }
        if (!header_parsed) {
            result.header = ParseCsvHeader(line);
            plan = BuildTickRowParsePlan(result.header);
            header_parsed = true;
            continue;
        }
        if (auto row = ParseTickRow(result.header, plan, line);
            row.has_value()) {
            result.rows.push_back(std::move(*row));
        }
    }
    return result;
}

}  // namespace svg_mb_control::analyze
