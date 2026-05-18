#include "analyze_csv.h"

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

const std::string& GetField(const std::vector<std::string>& fields,
                            const CsvHeader& header,
                            const std::string& name) {
    static const std::string kEmpty;
    auto it = header.column_index.find(name);
    if (it == header.column_index.end()) {
        return kEmpty;
    }
    if (it->second >= fields.size()) {
        return kEmpty;
    }
    return fields[it->second];
}

bool HasColumn(const CsvHeader& header, const std::string& name) {
    return header.column_index.find(name) != header.column_index.end();
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

std::optional<ParsedTickRow> ParseTickRow(const CsvHeader& header,
                                          const std::string& line) {
    if (line.empty()) {
        return std::nullopt;
    }
    const std::vector<std::string> fields = ParseCsvLine(line);
    if (fields.size() < 2u) {
        return std::nullopt;
    }

    ParsedTickRow row;
    row.wall_clock = GetField(fields, header, "wall_clock");
    if (row.wall_clock.empty()) {
        return std::nullopt;
    }
    if (auto tick = AsInt(GetField(fields, header, "loop_tick_count"));
        tick.has_value()) {
        row.tick_count = *tick;
    } else {
        return std::nullopt;
    }

    row.mode = AsText(GetField(fields, header, "mode"));
    row.snapshot_time = AsText(GetField(fields, header, "snapshot_time"));
    row.snapshot_age_ms = AsInt(GetField(fields, header, "snapshot_age_ms"));
    row.amd_sensor_count = AsInt(GetField(fields, header, "amd_sensor_count"));
    row.amd_sensor_summary = AsText(
        GetField(fields, header, "amd_sensor_summary"));
    row.cpu_tctl_c = AsDouble(GetField(fields, header, "cpu_tctl_c"));
    row.cpu_max_c = AsDouble(GetField(fields, header, "cpu_max_c"));
    row.gpu_available = AsInt(GetField(fields, header, "gpu_available"));
    row.gpu_name = AsText(GetField(fields, header, "gpu_name"));
    row.gpu_last_warning = AsText(
        GetField(fields, header, "gpu_last_warning"));
    row.gpu_core_c = AsDouble(GetField(fields, header, "gpu_core_c"));
    row.gpu_memjn_c = AsDouble(GetField(fields, header, "gpu_memjn_c"));
    row.gpu_hotspot_c = AsDouble(GetField(fields, header, "gpu_hotspot_c"));
    row.gpu_envelope_c = AsDouble(GetField(fields, header, "gpu_envelope_c"));
    if (!row.gpu_envelope_c.has_value()) {
        row.gpu_envelope_c = GpuEnvelopeC(
            row.gpu_core_c, row.gpu_memjn_c, row.gpu_hotspot_c);
    }
    row.fan_count = AsInt(GetField(fields, header, "fan_count"));
    row.policy_writes_enabled_present = AsInt(
        GetField(fields, header, "policy_writes_enabled_present"));
    row.policy_writes_enabled = AsInt(
        GetField(fields, header, "policy_writes_enabled"));
    row.loop_started_wall_clock = AsText(
        GetField(fields, header, "loop_started_wall_clock"));
    row.loop_finished_wall_clock = AsText(
        GetField(fields, header, "loop_finished_wall_clock"));
    row.loop_work_duration_ms = AsDouble(
        GetField(fields, header, "loop_work_duration_ms"));
    row.loop_intended_interval_ms = AsInt(
        GetField(fields, header, "loop_intended_interval_ms"));
    row.loop_achieved_interval_ms = AsDouble(
        GetField(fields, header, "loop_achieved_interval_ms"));
    row.loop_slip_ms = AsDouble(GetField(fields, header, "loop_slip_ms"));
    row.loop_overrun = AsInt(GetField(fields, header, "loop_overrun"));
    row.process_cpu_delta_ms = AsDouble(
        GetField(fields, header, "process_cpu_delta_ms"));
    row.process_cpu_pct = AsDouble(
        GetField(fields, header, "process_cpu_pct"));
    row.process_working_set_bytes = AsInt(
        GetField(fields, header, "process_working_set_bytes"));
    row.process_private_bytes = AsInt(
        GetField(fields, header, "process_private_bytes"));

    for (std::uint32_t fi = 0u; fi < 64u; ++fi) {
        const std::string prefix = "fan" + std::to_string(fi) + "_";
        if (!HasColumn(header, prefix + "present")) {
            break;
        }
        ParsedFanSample fan;
        fan.fan_index = fi;
        fan.present = AsInt(GetField(fields, header, prefix + "present"));
        fan.label = AsText(GetField(fields, header, prefix + "label"));
        fan.rpm = AsInt(GetField(fields, header, prefix + "rpm"));
        fan.tach_raw = AsInt(GetField(fields, header, prefix + "tach_raw"));
        fan.tach_valid = AsInt(GetField(fields, header, prefix + "tach_valid"));
        fan.duty_raw = AsInt(GetField(fields, header, prefix + "duty_raw"));
        fan.duty_pct = AsDouble(GetField(fields, header, prefix + "duty_pct"));
        fan.mode_raw = AsInt(GetField(fields, header, prefix + "mode_raw"));
        fan.manual_override = AsInt(
            GetField(fields, header, prefix + "manual_override"));
        fan.write_allowed = AsInt(
            GetField(fields, header, prefix + "write_allowed"));
        fan.policy_blocked = AsInt(
            GetField(fields, header, prefix + "policy_blocked"));
        fan.effective_write_allowed = AsInt(
            GetField(fields, header, prefix + "effective_write_allowed"));
        row.fans.push_back(std::move(fan));
    }

    for (std::uint32_t ci = 0u; ci < 64u; ++ci) {
        const std::string prefix = "channel" + std::to_string(ci) + "_";
        if (!HasColumn(header, prefix + "observed_temp_c")) {
            break;
        }
        ParsedChannelSample ch;
        ch.channel = ci;
        ch.observed_temp_c = AsDouble(
            GetField(fields, header, prefix + "observed_temp_c"));
        ch.setpoint_pct = AsDouble(
            GetField(fields, header, prefix + "setpoint_pct"));
        ch.thermal_pressure_boost_pct = AsDouble(
            GetField(fields, header, prefix + "thermal_pressure_boost_pct"));
        ch.cpu_low_soak_boost_pct = AsDouble(
            GetField(fields, header, prefix + "cpu_low_soak_boost_pct"));
        ch.response_source = AsText(
            GetField(fields, header, prefix + "response_source"));
        ch.write_reason = AsText(
            GetField(fields, header, prefix + "write_reason"));
        ch.total_writes = AsInt(
            GetField(fields, header, prefix + "total_writes"));
        ch.write_active = AsInt(
            GetField(fields, header, prefix + "write_active"));
        ch.baseline_captured = AsInt(
            GetField(fields, header, prefix + "baseline_captured"));
        ch.feedforward_pct = AsDouble(
            GetField(fields, header, prefix + "feedforward_pct"));
        ch.correction_pct = AsDouble(
            GetField(fields, header, prefix + "correction_pct"));
        row.channels.push_back(std::move(ch));
    }

    return row;
}

ParsedCsv ParseControlLoopCsv(const std::filesystem::path& path) {
    ParsedCsv result;
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("Failed to open CSV: " + path.string());
    }

    std::string line;
    bool header_parsed = false;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (line[0] == '#') {
            continue;
        }
        if (!header_parsed) {
            result.header = ParseCsvHeader(line);
            header_parsed = true;
            continue;
        }
        if (auto row = ParseTickRow(result.header, line); row.has_value()) {
            result.rows.push_back(std::move(*row));
        }
    }
    return result;
}

}  // namespace svg_mb_control::analyze
