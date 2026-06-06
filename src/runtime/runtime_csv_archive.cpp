#include "runtime_csv_archive.h"

#include "file_hash.h"
#include "json_io.h"
#include "runtime_event_log.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <ctime>
#include <iostream>
#include <ostream>
#include <sstream>
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

std::string SanitizePrologueValue(std::string value) {
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n') {
            ch = ' ';
        }
    }
    return value;
}

void WritePrologueField(std::ostream& stream,
                        std::string_view key,
                        const std::string& value) {
    stream << "# " << key << '=' << SanitizePrologueValue(value) << '\n';
}

void WriteOptionalPrologueField(std::ostream& stream,
                                std::string_view key,
                                const std::optional<std::uint32_t>& value) {
    if (value.has_value()) {
        WritePrologueField(stream, key, std::to_string(*value));
    }
}

struct ManifestPayloadInputs {
    std::string_view status;
    bool terminal_status;
    const std::string& mode;
    std::chrono::system_clock::time_point opened_at;
    const std::string& now_iso;
    std::uint64_t row_count;
    std::uint64_t event_count;
    const std::filesystem::path& active_archive_path;
    const std::filesystem::path& active_manifest_path;
    const std::filesystem::path& mirror_path;
    const std::filesystem::path& manifest_path;
    const std::filesystem::path& event_log_path;
    const RuntimeCsvIdentity& identity;
    const std::string& config_sha256;
    const std::string& runtime_policy_sha256;
    std::uint32_t csv_flush_interval_rows;
};

nlohmann::json IdentityFileJson(const std::filesystem::path& path,
                                const std::string& sha256) {
    return {
        {"path", path.empty() ? nlohmann::json(nullptr)
                              : nlohmann::json(path.string())},
        {"sha256", sha256.empty() ? nlohmann::json(nullptr)
                                  : nlohmann::json(sha256)},
    };
}

// Pure assembly of the runtime manifest JSON payload. No I/O and no member
// access; all state flows in via the inputs struct so WriteManifest can stay
// focused on flush + the two atomic file writes.
nlohmann::json BuildManifestPayload(const ManifestPayloadInputs& in) {
    return {
        {"schema", "svg_mb_control.runtime_log_manifest.v1"},
        {"status", std::string(in.status)},
        {"mode", in.mode},
        {"session_start", FormatLocalIso8601(in.opened_at)},
        {"session_stop",
         in.terminal_status ? nlohmann::json(in.now_iso)
                            : nlohmann::json(nullptr)},
        {"last_update", in.now_iso},
        {"row_count", in.row_count},
        {"event_count", in.event_count},
        {"rows_written",
         in.terminal_status ? nlohmann::json(in.row_count)
                            : nlohmann::json(nullptr)},
        {"events_written",
         in.terminal_status ? nlohmann::json(in.event_count)
                            : nlohmann::json(nullptr)},
        {"total_rows",
         in.terminal_status ? nlohmann::json(in.row_count)
                            : nlohmann::json(nullptr)},
        {"producer",
         {
             {"tool", "svg-mb-control"},
             {"version", SVG_MB_CONTROL_VERSION},
             {"git_hash", SVG_MB_CONTROL_GIT_HASH},
         }},
        {"config", IdentityFileJson(in.identity.config_path, in.config_sha256)},
        {"runtime_policy",
         IdentityFileJson(in.identity.runtime_policy_path,
                          in.runtime_policy_sha256)},
        {"control_loop",
         {
             {"poll_tick_ms",
              in.identity.control_poll_tick_ms
                  ? nlohmann::json(*in.identity.control_poll_tick_ms)
                  : nlohmann::json(nullptr)},
             {"write_cooldown_ms",
              in.identity.control_write_cooldown_ms
                  ? nlohmann::json(*in.identity.control_write_cooldown_ms)
                  : nlohmann::json(nullptr)},
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
                  {"path", in.active_archive_path.string()},
                  {"schema", "svg_mb_control.log.v1"},
              }},
             {"csv_latest",
              {
                  {"path", in.mirror_path.string()},
                  {"schema", "svg_mb_control.log.v1"},
              }},
             {"events",
              {
                  {"path", in.event_log_path.string()},
                  {"schema", "svg_mb_control.event.v1"},
              }},
             {"manifest_archive",
              {
                  {"path", in.active_manifest_path.string()},
                  {"schema", "svg_mb_control.runtime_log_manifest.v1"},
              }},
             {"manifest_latest",
              {
                  {"path", in.manifest_path.string()},
                  {"schema", "svg_mb_control.runtime_log_manifest.v1"},
              }},
         }},
        {"writer",
         {
             {"csv_flush_policy",
              in.csv_flush_interval_rows == 1u ? "per_row" : "row_interval"},
             {"csv_flush_interval_rows", in.csv_flush_interval_rows},
             {"mirror_mode",
              in.csv_flush_interval_rows == 1u ? "write_through"
                                                : "buffered_same_interval"},
         }},
    };
}

}  // namespace

RuntimeCsvLogger::RuntimeCsvLogger(std::filesystem::path runtime_home,
                                   std::uint32_t rotate_hours,
                                   std::uint32_t retain_days,
                                   std::uint32_t csv_flush_interval_rows,
                                   RuntimeArtifactNaming naming,
                                   RuntimeCsvIdentity identity)
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
      naming_(std::move(naming)),
      identity_(std::move(identity)) {}

RuntimeCsvLogger::~RuntimeCsvLogger() {
    Close();
}

bool RuntimeCsvLogger::Open(std::string_view mode,
                            std::string_view header_line) {
    mode_ = std::string(mode);
    header_line_ = std::string(header_line);
    ResolveIdentityHashes();
    return OpenNewChunk();
}

void RuntimeCsvLogger::ResolveIdentityHashes() {
    config_sha256_ = Sha256FileHex(identity_.config_path);
    runtime_policy_sha256_ = Sha256FileHex(identity_.runtime_policy_path);
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
    const std::string opened_iso = FormatLocalIso8601(opened_at_);
    for (std::ostream* stream : std::array<std::ostream*, 2>{
             &archive_stream_, &mirror_stream_}) {
        *stream << "# schema=svg_mb_control.log.v1\n";
        *stream << "# mode=" << mode_ << '\n';
        *stream << "# session_start=" << opened_iso << '\n';
        WritePrologueField(*stream, "producer", "svg-mb-control");
        WritePrologueField(*stream, "build_version", SVG_MB_CONTROL_VERSION);
        WritePrologueField(*stream, "git_hash", SVG_MB_CONTROL_GIT_HASH);
        WritePrologueField(*stream, "config_path",
                           identity_.config_path.string());
        WritePrologueField(*stream, "config_sha256", config_sha256_);
        WritePrologueField(*stream, "runtime_policy_path",
                           identity_.runtime_policy_path.string());
        WritePrologueField(*stream, "runtime_policy_sha256",
                           runtime_policy_sha256_);
        WriteOptionalPrologueField(*stream, "control_poll_tick_ms",
                                   identity_.control_poll_tick_ms);
        WriteOptionalPrologueField(*stream, "control_write_cooldown_ms",
                                   identity_.control_write_cooldown_ms);
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
        FormatLocalIso8601(std::chrono::system_clock::now());

    const nlohmann::json payload = BuildManifestPayload({
        .status = status,
        .terminal_status = terminal_status,
        .mode = mode_,
        .opened_at = opened_at_,
        .now_iso = now_iso,
        .row_count = row_count_,
        .event_count = event_count,
        .active_archive_path = active_archive_path_,
        .active_manifest_path = active_manifest_path_,
        .mirror_path = mirror_path_,
        .manifest_path = manifest_path_,
        .event_log_path = event_log_path,
        .identity = identity_,
        .config_sha256 = config_sha256_,
        .runtime_policy_sha256 = runtime_policy_sha256_,
        .csv_flush_interval_rows = csv_flush_interval_rows_,
    });

    std::string active_error;
    std::string manifest_error;
    const bool active_ok =
        TryWriteJsonFileAtomic(active_manifest_path_, payload, 2,
                               &active_error);
    const bool manifest_ok =
        TryWriteJsonFileAtomic(manifest_path_, payload, 2, &manifest_error);
    if (!active_ok || !manifest_ok) {
        std::cerr << "warning: failed to write runtime log manifest under "
                  << runtime_home_.string() << '\n';
        if (!active_ok) {
            std::cerr << "  active_manifest: "
                      << active_manifest_path_.string() << '\n'
                      << "  detail: " << active_error << '\n';
        }
        if (!manifest_ok) {
            std::cerr << "  latest_manifest: " << manifest_path_.string()
                      << '\n'
                      << "  detail: " << manifest_error << '\n';
        }
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
