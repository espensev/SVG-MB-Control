#include "runtime_csv_archive.h"

#include "json_io.h"
#include "runtime_event_log.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <ctime>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>

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

}  // namespace

RuntimeCsvLogger::RuntimeCsvLogger(std::filesystem::path runtime_home,
                                   std::uint32_t rotate_hours,
                                   std::uint32_t retain_days,
                                   std::uint32_t csv_flush_interval_rows,
                                   RuntimeArtifactNaming naming)
    : runtime_home_(std::move(runtime_home)),
      logs_dir_(ResolveRuntimeLogsDir(runtime_home_)),
      archive_dir_(ResolveRuntimeArchiveDir(runtime_home_)),
      mirror_path_(ResolveRuntimeLogMirrorPath(runtime_home_, naming)),
      manifest_path_(ResolveRuntimeLogManifestPath(runtime_home_, naming)),
      rotate_hours_(rotate_hours),
      retain_days_(retain_days),
      csv_flush_interval_rows_(csv_flush_interval_rows == 0u
                                    ? 1u
                                    : csv_flush_interval_rows),
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
    rows_since_flush_ = 0u;
    mirror_pending_rows_.clear();

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
    }
    FlushStreams();
}

bool RuntimeCsvLogger::FlushStreams() {
    if (!archive_stream_.is_open() && !mirror_stream_.is_open()) {
        return false;
    }

    if (archive_stream_.is_open()) {
        archive_stream_.flush();
        if (!archive_stream_.good()) {
            return false;
        }
    }
    if (mirror_stream_.is_open()) {
        if (!mirror_pending_rows_.empty()) {
            mirror_stream_ << mirror_pending_rows_;
            if (!mirror_stream_.good()) {
                return false;
            }
            mirror_pending_rows_.clear();
        }
        mirror_stream_.flush();
        if (!mirror_stream_.good()) {
            return false;
        }
    }
    rows_since_flush_ = 0u;
    return true;
}

void RuntimeCsvLogger::WriteManifest(std::string_view status) {
    if (active_archive_path_.empty() || active_manifest_path_.empty()) {
        return;
    }
    if (status == "running" && rows_since_flush_ > 0u && is_open()) {
        FlushStreams();
    }

    const std::filesystem::path event_log_path =
        ResolveRuntimeEventLogPath(runtime_home_, naming_);
    const bool terminal_status = status != "running";
    const std::uint64_t event_count =
        CachedEventCount(event_log_path, terminal_status);
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
             {"csv_flush_policy",
              csv_flush_interval_rows_ == 1u ? "per_row" : "row_interval"},
             {"csv_flush_interval_rows", csv_flush_interval_rows_},
             {"mirror_mode",
              csv_flush_interval_rows_ == 1u ? "write_through"
                                              : "buffered_same_interval"},
         }},
    };

    const bool active_ok =
        TryWriteJsonFileAtomic(active_manifest_path_, payload);
    const bool manifest_ok = TryWriteJsonFileAtomic(manifest_path_, payload);
    if (!active_ok || !manifest_ok) {
        std::cerr << "warning: failed to write runtime log manifest under "
                  << runtime_home_.string() << '\n';
    }
}

void RuntimeCsvLogger::CloseActiveChunk(std::string_view status) {
    const bool had_open_stream =
        archive_stream_.is_open() || mirror_stream_.is_open();
    if (had_open_stream) {
        FlushStreams();
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
    if (!archive_stream_.good()) {
        return false;
    }
    mirror_pending_rows_.append(row);
    mirror_pending_rows_.push_back('\n');
    ++row_count_;
    ++rows_since_flush_;
    if (rows_since_flush_ >= csv_flush_interval_rows_ && !FlushStreams()) {
        return false;
    }
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

}  // namespace svg_mb_control
