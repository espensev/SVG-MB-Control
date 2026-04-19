#include "runtime_logging.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>

namespace svg_mb_control {

namespace {

std::string FormatLocalIso8601(std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
    if (localtime_s(&local, &tt) != 0) {
        return {};
    }
    std::array<char, 32> buffer{};
    const std::size_t written = std::strftime(buffer.data(), buffer.size(),
                                              "%Y-%m-%dT%H:%M:%S", &local);
    return written > 0u ? std::string(buffer.data(), written) : std::string();
}

std::string FormatArchiveTimestamp(std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
    if (localtime_s(&local, &tt) != 0) {
        return {};
    }
    std::array<char, 32> buffer{};
    const std::size_t written = std::strftime(buffer.data(), buffer.size(),
                                              "%Y%m%d_%H%M%S", &local);
    return written > 0u ? std::string(buffer.data(), written) : std::string();
}

std::optional<std::chrono::system_clock::time_point> ParseLocalIso8601(
    std::string_view iso_text) {
    if (iso_text.empty()) {
        return std::nullopt;
    }

    std::tm tm_value{};
    std::istringstream stream{std::string(iso_text)};
    stream >> std::get_time(&tm_value, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) {
        return std::nullopt;
    }
    tm_value.tm_isdst = -1;
    const std::time_t tt = std::mktime(&tm_value);
    if (tt == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(tt);
}

std::string CsvEscape(std::string_view text) {
    bool needs_quotes = false;
    for (const char ch : text) {
        if (ch == '"' || ch == ',' || ch == '\n' || ch == '\r') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return std::string(text);
    }

    std::string output;
    output.reserve(text.size() + 4u);
    output.push_back('"');
    for (const char ch : text) {
        if (ch == '"') {
            output += "\"\"";
        } else {
            output.push_back(ch);
        }
    }
    output.push_back('"');
    return output;
}

void AppendCsvString(std::ostringstream& csv, std::string_view text) {
    csv << CsvEscape(text);
}

void AppendCsvDouble(std::ostringstream& csv, double value, int precision = 3) {
    if (std::isnan(value)) {
        return;
    }
    const std::streamsize old_precision = csv.precision();
    const auto old_flags = csv.flags();
    csv.setf(std::ios::fixed, std::ios::floatfield);
    csv.precision(precision);
    csv << value;
    csv.flags(old_flags);
    csv.precision(old_precision);
}

void AppendCsvBool(std::ostringstream& csv, bool value) {
    csv << (value ? "true" : "false");
}

double FindMaxCpuTemperature(const RuntimeSnapshot& snapshot) {
    double max_temp = std::numeric_limits<double>::quiet_NaN();
    for (const auto& sensor : snapshot.amd_sensors) {
        if (std::isnan(max_temp) || sensor.temperature_c > max_temp) {
            max_temp = sensor.temperature_c;
        }
    }
    return max_temp;
}

const RuntimeControlChannelLogState* FindChannelState(
    const std::vector<RuntimeControlChannelLogState>& channels,
    std::uint32_t channel) {
    for (const auto& entry : channels) {
        if (entry.channel == channel) {
            return &entry;
        }
    }
    return nullptr;
}

std::string BuildAmdSensorSummary(const RuntimeSnapshot& snapshot) {
    std::ostringstream summary;
    bool first = true;
    for (const auto& sensor : snapshot.amd_sensors) {
        if (!first) {
            summary << " | ";
        }
        first = false;
        summary << sensor.label << '=';
        const std::streamsize old_precision = summary.precision();
        const auto old_flags = summary.flags();
        summary.setf(std::ios::fixed, std::ios::floatfield);
        summary.precision(3);
        summary << sensor.temperature_c;
        summary.flags(old_flags);
        summary.precision(old_precision);
    }
    return summary.str();
}

std::string JsonEscape(std::string_view text) {
    std::string output;
    output.reserve(text.size() + 8u);
    for (const char ch : text) {
        switch (ch) {
            case '\\': output += "\\\\"; break;
            case '"': output += "\\\""; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20u) {
                    std::array<char, 8> buffer{};
                    std::snprintf(buffer.data(), buffer.size(), "\\u%04x",
                                  static_cast<unsigned int>(
                                      static_cast<unsigned char>(ch)));
                    output += buffer.data();
                } else {
                    output.push_back(ch);
                }
                break;
        }
    }
    return output;
}

std::string BuildCommonCsvHeader() {
    std::ostringstream header;
    header
        << "wall_clock,mode,snapshot_time,snapshot_age_ms,"
        << "amd_sensor_count,amd_sensor_summary,cpu_tctl_c,cpu_max_c,"
        << "gpu_available,gpu_name,gpu_last_warning,"
        << "gpu_core_c,gpu_memjn_c,gpu_hotspot_c,"
        << "fan_count,policy_writes_enabled_present,policy_writes_enabled";
    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        header << ",fan" << channel << "_present"
               << ",fan" << channel << "_label"
               << ",fan" << channel << "_rpm"
               << ",fan" << channel << "_tach_raw"
               << ",fan" << channel << "_tach_valid"
               << ",fan" << channel << "_duty_raw"
               << ",fan" << channel << "_duty_pct"
               << ",fan" << channel << "_mode_raw"
               << ",fan" << channel << "_manual_override"
               << ",fan" << channel << "_write_allowed"
               << ",fan" << channel << "_policy_blocked"
               << ",fan" << channel << "_effective_write_allowed";
    }
    return header.str();
}

std::string BuildCommonCsvPrefix(const RuntimeSnapshot& snapshot,
                                 std::string_view mode) {
    std::ostringstream csv;
    AppendCsvString(csv, FormatLocalIso8601(std::chrono::system_clock::now()));
    csv << ',';
    AppendCsvString(csv, mode);
    csv << ',';
    AppendCsvString(csv, snapshot.snapshot_time_iso);
    csv << ',';
    if (const auto parsed = ParseLocalIso8601(snapshot.snapshot_time_iso)) {
        const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now() - *parsed).count();
        csv << age_ms;
    }
    csv << ',' << snapshot.amd_sensors.size() << ',';
    AppendCsvString(csv, BuildAmdSensorSummary(snapshot));
    csv << ',';
    AppendCsvDouble(csv, FindRuntimeAmdSensorTemperature(snapshot, "Tctl/Tdie"));
    csv << ',';
    AppendCsvDouble(csv, FindMaxCpuTemperature(snapshot));
    csv << ',';
    AppendCsvBool(csv, snapshot.gpu.available);
    csv << ',';
    AppendCsvString(csv, snapshot.gpu.gpu_name);
    csv << ',';
    AppendCsvString(csv, snapshot.gpu.last_warning);
    csv << ',';
    AppendCsvDouble(csv, snapshot.gpu.core_c);
    csv << ',';
    AppendCsvDouble(csv, snapshot.gpu.memjn_c);
    csv << ',';
    AppendCsvDouble(csv, snapshot.gpu.hotspot_c);
    csv << ',' << snapshot.fans.size() << ',';
    AppendCsvBool(csv, snapshot.policy_writes_enabled_present);
    csv << ',';
    if (snapshot.policy_writes_enabled_present) {
        AppendCsvBool(csv, snapshot.policy_writes_enabled);
    }

    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        const RuntimeFanSnapshot* fan = FindRuntimeFanChannel(snapshot, channel);
        csv << ',';
        AppendCsvBool(csv, fan != nullptr);
        csv << ',';
        if (fan != nullptr) {
            AppendCsvString(csv, fan->label);
        }
        csv << ',';
        if (fan != nullptr) {
            csv << fan->rpm;
        }
        csv << ',';
        if (fan != nullptr) {
            csv << fan->tach_raw;
        }
        csv << ',';
        if (fan != nullptr) {
            AppendCsvBool(csv, fan->tach_valid);
        }
        csv << ',';
        if (fan != nullptr) {
            csv << static_cast<unsigned int>(fan->duty_raw);
        }
        csv << ',';
        if (fan != nullptr) {
            AppendCsvDouble(csv, fan->duty_percent, 2);
        }
        csv << ',';
        if (fan != nullptr) {
            csv << static_cast<unsigned int>(fan->mode_raw);
        }
        csv << ',';
        if (fan != nullptr) {
            AppendCsvBool(csv, fan->manual_override);
        }
        csv << ',';
        if (fan != nullptr) {
            AppendCsvBool(csv, fan->write_allowed);
        }
        csv << ',';
        if (fan != nullptr) {
            AppendCsvBool(csv, fan->policy_blocked);
        }
        csv << ',';
        if (fan != nullptr) {
            AppendCsvBool(csv, fan->effective_write_allowed);
        }
    }

    return csv.str();
}

}  // namespace

std::filesystem::path ResolveRuntimeLogsDir(
    const std::filesystem::path& runtime_home) {
    return runtime_home / "logs";
}

std::filesystem::path ResolveRuntimeArchiveDir(
    const std::filesystem::path& runtime_home) {
    return ResolveRuntimeLogsDir(runtime_home) / "archive";
}

std::filesystem::path ResolveRuntimeLogMirrorPath(
    const std::filesystem::path& runtime_home) {
    return ResolveRuntimeLogsDir(runtime_home) / "svg_mb_control_output.csv";
}

std::filesystem::path ResolveRuntimeEventLogPath(
    const std::filesystem::path& runtime_home) {
    return ResolveRuntimeLogsDir(runtime_home) / "svg_mb_control_events.jsonl";
}

RuntimeCsvLogger::RuntimeCsvLogger(std::filesystem::path runtime_home,
                                   std::uint32_t rotate_hours,
                                   std::uint32_t retain_days)
    : runtime_home_(std::move(runtime_home)),
      logs_dir_(ResolveRuntimeLogsDir(runtime_home_)),
      archive_dir_(ResolveRuntimeArchiveDir(runtime_home_)),
      mirror_path_(ResolveRuntimeLogMirrorPath(runtime_home_)),
      rotate_hours_(rotate_hours),
      retain_days_(retain_days) {}

RuntimeCsvLogger::~RuntimeCsvLogger() {
    Close();
}

bool RuntimeCsvLogger::Open(std::string_view mode, std::string_view header_line) {
    mode_ = std::string(mode);
    header_line_ = std::string(header_line);
    return OpenNewChunk();
}

bool RuntimeCsvLogger::OpenNewChunk() {
    Close();

    std::error_code ec;
    std::filesystem::create_directories(archive_dir_, ec);
    if (ec) {
        return false;
    }
    std::filesystem::create_directories(logs_dir_, ec);
    if (ec) {
        return false;
    }

    opened_at_ = std::chrono::system_clock::now();
    active_archive_path_ = archive_dir_ /
        ("svg_mb_control_" + mode_ + "_" + FormatArchiveTimestamp(opened_at_) +
         ".csv");

    archive_stream_.open(active_archive_path_,
                         std::ios::binary | std::ios::trunc);
    if (!archive_stream_.is_open()) {
        active_archive_path_.clear();
        return false;
    }
    mirror_stream_.open(mirror_path_, std::ios::binary | std::ios::trunc);
    if (!mirror_stream_.is_open()) {
        archive_stream_.close();
        active_archive_path_.clear();
        return false;
    }

    WritePrologue();
    PruneOldArchives();
    return archive_stream_.good() && mirror_stream_.good();
}

void RuntimeCsvLogger::WritePrologue() {
    if (!archive_stream_.is_open() || !mirror_stream_.is_open()) {
        return;
    }
    const std::string opened_iso = FormatLocalIso8601(opened_at_);
    for (std::ostream* stream : std::array<std::ostream*, 2>{
             &archive_stream_, &mirror_stream_}) {
        *stream << "# schema=svg_mb_control.log.v1\n";
        *stream << "# mode=" << mode_ << '\n';
        *stream << "# session_start=" << opened_iso << '\n';
        *stream << header_line_ << '\n';
        stream->flush();
    }
}

void RuntimeCsvLogger::PruneOldArchives() {
    if (retain_days_ == 0u) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(archive_dir_, ec)) {
        return;
    }

    const auto cutoff =
        std::filesystem::file_time_type::clock::now() -
        std::chrono::hours(24 * retain_days_);
    for (const auto& entry :
         std::filesystem::directory_iterator(archive_dir_, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (entry.path() == active_archive_path_) {
            continue;
        }
        if (entry.path().extension() != ".csv") {
            continue;
        }
        const auto mtime = entry.last_write_time(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (mtime < cutoff) {
            std::filesystem::remove(entry.path(), ec);
            ec.clear();
        }
    }
}

bool RuntimeCsvLogger::MaybeRotate() {
    if (!is_open() || rotate_hours_ == 0u) {
        return false;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::hours>(
        std::chrono::system_clock::now() - opened_at_);
    if (elapsed.count() < static_cast<long long>(rotate_hours_)) {
        return false;
    }
    return OpenNewChunk();
}

bool RuntimeCsvLogger::WriteRow(std::string_view row) {
    if (!is_open()) {
        return false;
    }

    archive_stream_ << row << '\n';
    mirror_stream_ << row << '\n';
    archive_stream_.flush();
    mirror_stream_.flush();
    return archive_stream_.good() && mirror_stream_.good();
}

void RuntimeCsvLogger::Close() {
    if (archive_stream_.is_open()) {
        archive_stream_.flush();
        archive_stream_.close();
    }
    if (mirror_stream_.is_open()) {
        mirror_stream_.flush();
        mirror_stream_.close();
    }
}

bool RuntimeCsvLogger::is_open() const {
    return archive_stream_.is_open() && mirror_stream_.is_open();
}

const std::filesystem::path& RuntimeCsvLogger::active_archive_path() const {
    return active_archive_path_;
}

const std::filesystem::path& RuntimeCsvLogger::mirror_path() const {
    return mirror_path_;
}

std::string BuildReadLoopCsvHeader() {
    std::ostringstream header;
    header << BuildCommonCsvHeader()
           << ",telemetry_available"
           << ",runtime_home_published"
           << ",snapshot_mirror_configured"
           << ",snapshot_mirror_published"
           << ",successful_polls"
           << ",skipped_polls"
           << ",stale"
           << ",status_detail";
    return header.str();
}

std::string BuildReadLoopCsvRow(const RuntimeSnapshot& snapshot,
                                const RuntimeReadLoopLogState& state) {
    std::ostringstream csv;
    csv << BuildCommonCsvPrefix(snapshot, "read-loop") << ',';
    AppendCsvBool(csv, state.telemetry_available);
    csv << ',';
    AppendCsvBool(csv, state.runtime_home_published);
    csv << ',';
    AppendCsvBool(csv, state.snapshot_mirror_configured);
    csv << ',';
    AppendCsvBool(csv, state.snapshot_mirror_published);
    csv << ',' << state.successful_polls
        << ',' << state.skipped_polls
        << ',';
    AppendCsvBool(csv, state.stale);
    csv << ',';
    AppendCsvString(csv, state.status_detail);
    return csv.str();
}

std::string BuildControlLoopCsvHeader() {
    std::ostringstream header;
    header << BuildCommonCsvHeader() << ",loop_tick_count";
    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        header << ",channel" << channel << "_observed_temp_c"
               << ",channel" << channel << "_setpoint_pct"
               << ",channel" << channel << "_total_writes"
               << ",channel" << channel << "_write_active"
               << ",channel" << channel << "_baseline_captured";
    }
    return header.str();
}

std::string BuildControlLoopCsvRow(
    const RuntimeSnapshot& snapshot,
    std::uint64_t tick_count,
    const std::vector<RuntimeControlChannelLogState>& channels) {
    std::ostringstream csv;
    csv << BuildCommonCsvPrefix(snapshot, "control-loop") << ',' << tick_count;
    for (std::uint32_t channel = 0u;
         channel < static_cast<std::uint32_t>(kRuntimeLogFanChannelCount);
         ++channel) {
        const RuntimeControlChannelLogState* state =
            FindChannelState(channels, channel);
        csv << ',';
        if (state != nullptr) {
            AppendCsvDouble(csv, state->observed_temp_c);
        }
        csv << ',';
        if (state != nullptr) {
            AppendCsvDouble(csv, state->setpoint_pct);
        }
        csv << ',';
        if (state != nullptr) {
            csv << state->total_writes;
        }
        csv << ',';
        if (state != nullptr) {
            AppendCsvBool(csv, state->write_active);
        }
        csv << ',';
        if (state != nullptr) {
            AppendCsvBool(csv, state->baseline_captured);
        }
    }
    return csv.str();
}

bool AppendRuntimeEvent(const std::filesystem::path& runtime_home,
                        const RuntimeLogEvent& event) {
    std::error_code ec;
    const std::filesystem::path path = ResolveRuntimeEventLogPath(runtime_home);
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }

    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (!stream.is_open()) {
        return false;
    }

    const std::string event_time = event.event_time_iso.empty()
        ? FormatLocalIso8601(std::chrono::system_clock::now())
        : event.event_time_iso;

    stream << "{"
           << "\"schema\":\"svg_mb_control.event.v1\","
           << "\"event_time\":\"" << JsonEscape(event_time) << "\","
           << "\"mode\":\"" << JsonEscape(event.mode) << "\","
           << "\"event_type\":\"" << JsonEscape(event.event_type) << "\","
           << "\"detail\":\"" << JsonEscape(event.detail) << "\"";
    if (event.channel.has_value()) {
        stream << ",\"channel\":" << *event.channel;
    }
    if (event.tick_count.has_value()) {
        stream << ",\"tick_count\":" << *event.tick_count;
    }
    if (event.observed_temp_c.has_value()) {
        stream << ",\"observed_temp_c\":" << *event.observed_temp_c;
    }
    if (event.setpoint_pct.has_value()) {
        stream << ",\"setpoint_pct\":" << *event.setpoint_pct;
    }
    if (event.target_pct.has_value()) {
        stream << ",\"target_pct\":" << *event.target_pct;
    }
    if (event.success.has_value()) {
        stream << ",\"success\":" << (*event.success ? "true" : "false");
    }
    if (!event.snapshot_time_iso.empty()) {
        stream << ",\"snapshot_time\":\""
               << JsonEscape(event.snapshot_time_iso) << "\"";
    }
    if (!event.log_csv_path.empty()) {
        stream << ",\"log_csv_path\":\""
               << JsonEscape(event.log_csv_path) << "\"";
    }
    if (!event.event_log_path.empty()) {
        stream << ",\"event_log_path\":\""
               << JsonEscape(event.event_log_path) << "\"";
    }
    if (event.amd_sensor_count.has_value()) {
        stream << ",\"amd_sensor_count\":" << *event.amd_sensor_count;
    }
    if (event.fan_count.has_value()) {
        stream << ",\"fan_count\":" << *event.fan_count;
    }
    if (event.gpu_available.has_value()) {
        stream << ",\"gpu_available\":"
               << (*event.gpu_available ? "true" : "false");
    }
    if (event.successful_polls.has_value()) {
        stream << ",\"successful_polls\":" << *event.successful_polls;
    }
    if (event.skipped_polls.has_value()) {
        stream << ",\"skipped_polls\":" << *event.skipped_polls;
    }
    if (event.stale.has_value()) {
        stream << ",\"stale\":" << (*event.stale ? "true" : "false");
    }
    if (event.telemetry_available.has_value()) {
        stream << ",\"telemetry_available\":"
               << (*event.telemetry_available ? "true" : "false");
    }
    if (event.runtime_home_published.has_value()) {
        stream << ",\"runtime_home_published\":"
               << (*event.runtime_home_published ? "true" : "false");
    }
    if (event.snapshot_mirror_configured.has_value()) {
        stream << ",\"snapshot_mirror_configured\":"
               << (*event.snapshot_mirror_configured ? "true" : "false");
    }
    if (event.snapshot_mirror_published.has_value()) {
        stream << ",\"snapshot_mirror_published\":"
               << (*event.snapshot_mirror_published ? "true" : "false");
    }
    stream << "}\n";
    stream.flush();
    return stream.good();
}

}  // namespace svg_mb_control
