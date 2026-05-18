#include "runtime_artifacts.h"

#include "json_io.h"

#include <array>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#ifndef SVG_MB_CONTROL_VERSION
#define SVG_MB_CONTROL_VERSION "unknown"
#endif

#ifndef SVG_MB_CONTROL_GIT_HASH
#define SVG_MB_CONTROL_GIT_HASH "unknown"
#endif

namespace svg_mb_control {

namespace {

constexpr std::uint64_t kRuntimeManifestUpdateIntervalRows = 100u;

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

std::uint64_t CountNonEmptyLines(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return 0u;
    }
    std::uint64_t count = 0u;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            ++count;
        }
    }
    return count;
}

void PutOptionalDouble(nlohmann::json& payload,
                       std::string_view key,
                       const std::optional<double>& value) {
    if (!value.has_value()) {
        return;
    }
    payload[std::string(key)] = std::isfinite(*value)
        ? nlohmann::json(*value)
        : nlohmann::json(nullptr);
}

}  // namespace

std::string FormatRuntimeLocalIso8601(
    std::chrono::system_clock::time_point tp) {
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
    return ResolveRuntimeLogMirrorPath(runtime_home, RuntimeArtifactNaming{});
}

std::filesystem::path ResolveRuntimeLogMirrorPath(
    const std::filesystem::path& runtime_home,
    const RuntimeArtifactNaming& naming) {
    return ResolveRuntimeLogsDir(runtime_home) / naming.latest_csv_name;
}

std::filesystem::path ResolveRuntimeEventLogPath(
    const std::filesystem::path& runtime_home) {
    return ResolveRuntimeEventLogPath(runtime_home, RuntimeArtifactNaming{});
}

std::filesystem::path ResolveRuntimeEventLogPath(
    const std::filesystem::path& runtime_home,
    const RuntimeArtifactNaming& naming) {
    return ResolveRuntimeLogsDir(runtime_home) / naming.latest_events_name;
}

std::filesystem::path ResolveRuntimeLogManifestPath(
    const std::filesystem::path& runtime_home) {
    return ResolveRuntimeLogManifestPath(runtime_home, RuntimeArtifactNaming{});
}

std::filesystem::path ResolveRuntimeLogManifestPath(
    const std::filesystem::path& runtime_home,
    const RuntimeArtifactNaming& naming) {
    return ResolveRuntimeLogsDir(runtime_home) / naming.latest_manifest_name;
}

RuntimeCsvLogger::RuntimeCsvLogger(std::filesystem::path runtime_home,
                                   std::uint32_t rotate_hours,
                                   std::uint32_t retain_days,
                                   RuntimeArtifactNaming naming)
    : runtime_home_(std::move(runtime_home)),
      logs_dir_(ResolveRuntimeLogsDir(runtime_home_)),
      archive_dir_(ResolveRuntimeArchiveDir(runtime_home_)),
      mirror_path_(ResolveRuntimeLogMirrorPath(runtime_home_, naming)),
      manifest_path_(ResolveRuntimeLogManifestPath(runtime_home_, naming)),
      rotate_hours_(rotate_hours),
      retain_days_(retain_days),
      naming_(std::move(naming)) {}

RuntimeCsvLogger::~RuntimeCsvLogger() {
    Close();
}

bool RuntimeCsvLogger::Open(std::string_view mode,
                            std::string_view header_line) {
    mode_ = std::string(mode);
    header_line_ = std::string(header_line);
    return OpenNewChunk();
}

bool RuntimeCsvLogger::OpenNewChunk() {
    CloseActiveChunk("rotated");

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
        (naming_.archive_prefix + "_" + mode_ + "_" +
         FormatArchiveTimestamp(opened_at_) +
         ".csv");
    active_manifest_path_ = active_archive_path_;
    active_manifest_path_.replace_extension(".manifest.json");
    row_count_ = 0u;

    archive_stream_.open(active_archive_path_,
                         std::ios::binary | std::ios::trunc);
    if (!archive_stream_.is_open()) {
        active_archive_path_.clear();
        active_manifest_path_.clear();
        return false;
    }
    mirror_stream_.open(mirror_path_, std::ios::binary | std::ios::trunc);
    if (!mirror_stream_.is_open()) {
        archive_stream_.close();
        active_archive_path_.clear();
        active_manifest_path_.clear();
        return false;
    }

    WritePrologue();
    WriteManifest("running");
    PruneOldArchives();
    return archive_stream_.good() && mirror_stream_.good();
}

void RuntimeCsvLogger::WritePrologue() {
    if (!archive_stream_.is_open() || !mirror_stream_.is_open()) {
        return;
    }
    const std::string opened_iso = FormatRuntimeLocalIso8601(opened_at_);
    for (std::ostream* stream : std::array<std::ostream*, 2>{
             &archive_stream_, &mirror_stream_}) {
        *stream << "# schema=svg_mb_control.log.v1\n";
        *stream << "# mode=" << mode_ << '\n';
        *stream << "# session_start=" << opened_iso << '\n';
        *stream << header_line_ << '\n';
        stream->flush();
    }
}

void RuntimeCsvLogger::WriteManifest(std::string_view status) {
    if (active_archive_path_.empty() || active_manifest_path_.empty()) {
        return;
    }

    const std::filesystem::path event_log_path =
        ResolveRuntimeEventLogPath(runtime_home_, naming_);
    const std::uint64_t event_count = CountNonEmptyLines(event_log_path);
    const bool terminal_status = status != "running";
    const std::string now_iso =
        FormatRuntimeLocalIso8601(std::chrono::system_clock::now());

    nlohmann::json payload = {
        {"schema", "svg_mb_control.runtime_log_manifest.v1"},
        {"status", std::string(status)},
        {"mode", mode_},
        {"session_start", FormatRuntimeLocalIso8601(opened_at_)},
        {"session_stop",
         terminal_status ? nlohmann::json(now_iso) : nlohmann::json(nullptr)},
        {"last_update", now_iso},
        {"row_count", row_count_},
        {"event_count", event_count},
        {"rows_written",
         terminal_status ? nlohmann::json(row_count_) : nlohmann::json(nullptr)},
        {"events_written",
         terminal_status ? nlohmann::json(event_count)
                         : nlohmann::json(nullptr)},
        {"total_rows",
         terminal_status ? nlohmann::json(row_count_) : nlohmann::json(nullptr)},
        {"producer",
         {
             {"tool", "svg-mb-control"},
             {"version", SVG_MB_CONTROL_VERSION},
             {"git_hash", SVG_MB_CONTROL_GIT_HASH},
         }},
        {"external_logging",
         {
             {"required", false},
             {"preferred_source", "svg-mb-control runtime CSV/JSONL"},
         }},
        {"artifacts",
         {
             {"csv_archive",
              {
                  {"path", active_archive_path_.string()},
                  {"schema", "svg_mb_control.log.v1"},
              }},
             {"csv_latest",
              {
                  {"path", mirror_path_.string()},
                  {"schema", "svg_mb_control.log.v1"},
              }},
             {"events",
              {
                  {"path", event_log_path.string()},
                  {"schema", "svg_mb_control.event.v1"},
              }},
             {"manifest_archive",
              {
                  {"path", active_manifest_path_.string()},
                  {"schema", "svg_mb_control.runtime_log_manifest.v1"},
              }},
             {"manifest_latest",
              {
                  {"path", manifest_path_.string()},
                  {"schema", "svg_mb_control.runtime_log_manifest.v1"},
              }},
         }},
        {"writer",
         {
             {"csv_flush_policy", "per_row"},
             {"mirror_mode", "write_through"},
         }},
    };

    TryWriteJsonFileAtomic(active_manifest_path_, payload);
    TryWriteJsonFileAtomic(manifest_path_, payload);
}

void RuntimeCsvLogger::CloseActiveChunk(std::string_view status) {
    const bool had_open_stream =
        archive_stream_.is_open() || mirror_stream_.is_open();
    if (archive_stream_.is_open()) {
        archive_stream_.flush();
    }
    if (mirror_stream_.is_open()) {
        mirror_stream_.flush();
    }
    if (had_open_stream) {
        WriteManifest(status);
    }
    if (archive_stream_.is_open()) {
        archive_stream_.close();
    }
    if (mirror_stream_.is_open()) {
        mirror_stream_.close();
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
            std::filesystem::path manifest_path = entry.path();
            manifest_path.replace_extension(".manifest.json");
            std::filesystem::remove(entry.path(), ec);
            if (ec) {
                ec.clear();
                continue;
            }
            if (manifest_path != active_manifest_path_) {
                std::filesystem::remove(manifest_path, ec);
                ec.clear();
            }
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
    if (!archive_stream_.good() || !mirror_stream_.good()) {
        return false;
    }
    ++row_count_;
    if ((row_count_ % kRuntimeManifestUpdateIntervalRows) == 0u) {
        WriteManifest("running");
    }
    return true;
}

void RuntimeCsvLogger::Close() {
    CloseActiveChunk("completed");
}

bool RuntimeCsvLogger::is_open() const {
    return archive_stream_.is_open() && mirror_stream_.is_open();
}

const std::filesystem::path& RuntimeCsvLogger::active_archive_path() const {
    return active_archive_path_;
}

const std::filesystem::path& RuntimeCsvLogger::active_manifest_path() const {
    return active_manifest_path_;
}

const std::filesystem::path& RuntimeCsvLogger::mirror_path() const {
    return mirror_path_;
}

const std::filesystem::path& RuntimeCsvLogger::manifest_path() const {
    return manifest_path_;
}

std::uint64_t RuntimeCsvLogger::row_count() const {
    return row_count_;
}

bool AppendRuntimeEvent(const std::filesystem::path& runtime_home,
                        const RuntimeLogEvent& event,
                        const RuntimeArtifactNaming& naming) {
    std::error_code ec;
    const std::filesystem::path path =
        ResolveRuntimeEventLogPath(runtime_home, naming);
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }

    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (!stream.is_open()) {
        return false;
    }

    const std::string event_time = event.event_time_iso.empty()
        ? FormatRuntimeLocalIso8601(std::chrono::system_clock::now())
        : event.event_time_iso;

    nlohmann::json payload = {
        {"schema", "svg_mb_control.event.v1"},
        {"event_time", event_time},
        {"mode", event.mode},
        {"event_type", event.event_type},
        {"detail", event.detail},
    };
    if (event.channel.has_value()) {
        payload["channel"] = *event.channel;
    }
    if (event.tick_count.has_value()) {
        payload["tick_count"] = *event.tick_count;
    }
    PutOptionalDouble(payload, "observed_temp_c", event.observed_temp_c);
    PutOptionalDouble(payload, "setpoint_pct", event.setpoint_pct);
    PutOptionalDouble(payload, "target_pct", event.target_pct);
    if (event.success.has_value()) {
        payload["success"] = *event.success;
    }
    if (!event.snapshot_time_iso.empty()) {
        payload["snapshot_time"] = event.snapshot_time_iso;
    }
    if (!event.log_csv_path.empty()) {
        payload["log_csv_path"] = event.log_csv_path;
    }
    if (!event.event_log_path.empty()) {
        payload["event_log_path"] = event.event_log_path;
    }
    if (event.amd_sensor_count.has_value()) {
        payload["amd_sensor_count"] = *event.amd_sensor_count;
    }
    if (event.fan_count.has_value()) {
        payload["fan_count"] = *event.fan_count;
    }
    if (event.gpu_available.has_value()) {
        payload["gpu_available"] = *event.gpu_available;
    }
    if (event.successful_polls.has_value()) {
        payload["successful_polls"] = *event.successful_polls;
    }
    if (event.skipped_polls.has_value()) {
        payload["skipped_polls"] = *event.skipped_polls;
    }
    if (event.stale.has_value()) {
        payload["stale"] = *event.stale;
    }
    if (event.telemetry_available.has_value()) {
        payload["telemetry_available"] = *event.telemetry_available;
    }
    if (event.runtime_home_published.has_value()) {
        payload["runtime_home_published"] = *event.runtime_home_published;
    }
    if (event.snapshot_mirror_configured.has_value()) {
        payload["snapshot_mirror_configured"] =
            *event.snapshot_mirror_configured;
    }
    if (event.snapshot_mirror_published.has_value()) {
        payload["snapshot_mirror_published"] =
            *event.snapshot_mirror_published;
    }
    stream << payload.dump() << '\n';
    stream.flush();
    return stream.good();
}

}  // namespace svg_mb_control
