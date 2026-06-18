#include "runtime_event_log.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "windows_lean.h"
#endif

namespace svg_mb_control {

namespace {

std::uint64_t CountNonEmptyLines(const std::filesystem::path& path) {
#ifdef _WIN32
    // Use CreateFileW with FILE_SHARE_DELETE so this read never blocks a
    // concurrent rename or rotation of the event log. AppendRuntimeEvent
    // opens the same file with FILE_SHARE_READ | FILE_SHARE_WRITE |
    // FILE_SHARE_DELETE; without matching share flags here, the future
    // rotator would hit ERROR_SHARING_VIOLATION mid-count.
    HANDLE handle = CreateFileW(
        path.wstring().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0u;
    }
    std::uint64_t count = 0u;
    bool current_has_content = false;
    constexpr DWORD kChunk = 64u * 1024u;
    std::vector<char> buffer(kChunk);
    for (;;) {
        DWORD bytes_read = 0u;
        if (!ReadFile(handle, buffer.data(), kChunk, &bytes_read, nullptr) ||
            bytes_read == 0u) {
            break;
        }
        for (DWORD i = 0u; i < bytes_read; ++i) {
            if (buffer[i] == '\n') {
                if (current_has_content) {
                    ++count;
                }
                current_has_content = false;
            } else {
                current_has_content = true;
            }
        }
    }
    if (current_has_content) {
        ++count;
    }
    CloseHandle(handle);
    return count;
#else
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
#endif
}

std::string EventCountKey(const std::filesystem::path& path) {
    // Lowercase the normalized path so case-different spellings of the same
    // Windows path collapse to one cache entry. Matches the canonicalization
    // used by the runtime singleton mutex name. POSIX paths are
    // case-sensitive at the FS layer, but lowercasing here is harmless and
    // keeps the keying rule single across platforms.
    std::string key = path.lexically_normal().string();
    for (char& ch : key) {
        ch = static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
    }
    return key;
}

struct EventLogRegistry {
    std::mutex mutex;
    std::unordered_map<std::string, std::uint64_t> counts;
    std::unordered_map<std::string, RuntimeEventLogOptions> options;
};

EventLogRegistry& RuntimeEventLogRegistry() {
    static EventLogRegistry registry;
    return registry;
}

void NoteEventAppendedLocked(EventLogRegistry& registry,
                             const std::filesystem::path& path,
                             const std::string& key) {
    auto existing = registry.counts.find(key);
    if (existing == registry.counts.end()) {
        registry.counts.emplace(key, CountNonEmptyLines(path));
        return;
    }
    ++existing->second;
}

void ResetEventCountLocked(EventLogRegistry& registry,
                           const std::string& key,
                           std::uint64_t value) {
    registry.counts[key] = value;
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

bool Contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

std::string NormalizeEventCode(std::string_view event_type) {
    std::string code;
    code.reserve(event_type.size());
    bool last_was_separator = false;
    for (unsigned char ch : event_type) {
        if (std::isalnum(ch)) {
            code.push_back(static_cast<char>(std::toupper(ch)));
            last_was_separator = false;
        } else if (!last_was_separator) {
            code.push_back('_');
            last_was_separator = true;
        }
    }
    while (!code.empty() && code.back() == '_') {
        code.pop_back();
    }
    return code.empty() ? "UNKNOWN_EVENT" : code;
}

std::string InferSeverity(const RuntimeLogEvent& event) {
    if (!event.severity.empty()) {
        return event.severity;
    }

    const std::string_view type(event.event_type);
    if (Contains(type, ".abort") || Contains(type, "fatal")) {
        return "critical";
    }
    if (Contains(type, "warning") || Contains(type, "skipped") ||
        Contains(type, "rejected") || Contains(type, "invalid_request") ||
        Contains(type, "circuit_breaker_opened") ||
        Contains(type, "sensor_failure_detected") ||
        Contains(type, "worker_restart_scheduled")) {
        return "warning";
    }
    if (event.success.has_value() && !*event.success) {
        return "error";
    }
    if (Contains(type, "failed") || Contains(type, "failure") ||
        Contains(type, "timeout")) {
        return "error";
    }
    return "info";
}

std::string InferErrorCode(const RuntimeLogEvent& event,
                           std::string_view severity) {
    if (!event.error_code.empty()) {
        return event.error_code;
    }
    if (severity == "info") {
        return "none";
    }
    return NormalizeEventCode(event.event_type);
}

std::string FormatEventArchiveTimestamp(
    std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &tt) != 0) {
        return {};
    }
#else
    if (localtime_r(&tt, &local) == nullptr) {
        return {};
    }
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d_%H%M%S");
    return out.str();
}

std::string EventArchiveStem(const std::filesystem::path& active_path) {
    const std::filesystem::path filename = active_path.filename();
    if (filename.extension() == ".jsonl") {
        return filename.stem().string();
    }
    return filename.string();
}

std::filesystem::path ResolveEventArchivePath(
    const std::filesystem::path& active_path,
    std::chrono::system_clock::time_point now) {
    const auto archive_dir = active_path.parent_path() / "archive";
    const std::string stem = EventArchiveStem(active_path);
    const std::string timestamp = FormatEventArchiveTimestamp(now);
    const std::string base = stem + "_" +
        (timestamp.empty() ? std::string("unknown") : timestamp);
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string name = base;
        if (attempt > 0) {
            name += "_";
            name += std::to_string(attempt);
        }
        name += ".jsonl";
        std::filesystem::path candidate = archive_dir / name;
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return archive_dir / (base + "_overflow.jsonl");
}

void PruneOldEventArchives(const std::filesystem::path& active_path,
                           const RuntimeEventLogOptions& options) {
    if (options.retain_days == 0u) {
        return;
    }
    const auto archive_dir = active_path.parent_path() / "archive";
    std::error_code ec;
    if (!std::filesystem::is_directory(archive_dir, ec) || ec) {
        return;
    }
    const std::string prefix = EventArchiveStem(active_path) + "_";
    const auto cutoff = std::filesystem::file_time_type::clock::now() -
                        std::chrono::hours(
                            24ll * static_cast<long long>(options.retain_days));
    for (const auto& entry :
         std::filesystem::directory_iterator(archive_dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const std::filesystem::path path = entry.path();
        const std::string name = path.filename().string();
        if (path.extension() != ".jsonl" ||
            name.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        const auto mtime = entry.last_write_time(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (mtime < cutoff) {
            std::filesystem::remove(path, ec);
            ec.clear();
        }
    }
}

void RotateEventLogIfDueLocked(EventLogRegistry& registry,
                               const std::filesystem::path& path,
                               const std::string& key,
                               const RuntimeEventLogOptions& options) {
    if (options.rotate_hours == 0u) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return;
    }
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return;
    }
    const auto cutoff = std::filesystem::file_time_type::clock::now() -
                        std::chrono::hours(options.rotate_hours);
    if (mtime >= cutoff) {
        return;
    }

    const auto archive_path =
        ResolveEventArchivePath(path, std::chrono::system_clock::now());
    std::filesystem::create_directories(archive_path.parent_path(), ec);
    if (ec) {
        return;
    }
    std::filesystem::rename(path, archive_path, ec);
    if (ec) {
        return;
    }
    ResetEventCountLocked(registry, key, 0u);
    PruneOldEventArchives(path, options);
}

bool ShouldPersistEvent(const RuntimeLogEvent& event,
                        std::string_view severity,
                        const RuntimeEventLogOptions& options) {
    if (!options.reduce_routine_write_applied ||
        event.event_type != "control_loop.write_applied" ||
        severity != "info") {
        return true;
    }
    if (!event.tick_count.has_value() || *event.tick_count <= 1u) {
        return true;
    }
    const std::uint32_t interval =
        options.write_applied_sample_interval == 0u
            ? 1u
            : options.write_applied_sample_interval;
    return (*event.tick_count % interval) == 0u;
}

bool AppendLineAtomic(const std::filesystem::path& path,
                      const std::string& line) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(
        path.wstring().c_str(),
        FILE_APPEND_DATA | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0u;
    const BOOL ok = WriteFile(
        handle,
        line.data(),
        static_cast<DWORD>(line.size()),
        &written,
        nullptr);
    CloseHandle(handle);
    return ok && written == line.size();
#else
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (!stream.is_open()) {
        return false;
    }
    stream << line;
    stream.flush();
    return stream.good();
#endif
}

}  // namespace

std::uint64_t CachedEventCount(const std::filesystem::path& path,
                               bool refresh_from_disk) {
    EventLogRegistry& registry = RuntimeEventLogRegistry();
    const std::string key = EventCountKey(path);
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto existing = registry.counts.find(key);
    if (!refresh_from_disk && existing != registry.counts.end()) {
        return existing->second;
    }

    const std::uint64_t count = CountNonEmptyLines(path);
    registry.counts[key] = count;
    return count;
}

void ConfigureRuntimeEventLogRetention(
    const std::filesystem::path& event_log_path,
    const RuntimeEventLogOptions& options) {
    EventLogRegistry& registry = RuntimeEventLogRegistry();
    const std::string key = EventCountKey(event_log_path);
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.options[key] = options;
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

    const std::string event_time = event.event_time_iso.empty()
        ? FormatLocalIso8601(std::chrono::system_clock::now())
        : event.event_time_iso;
    const std::string severity = InferSeverity(event);
    const std::string error_code = InferErrorCode(event, severity);
    const std::string key = EventCountKey(path);

    EventLogRegistry& registry = RuntimeEventLogRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    RuntimeEventLogOptions options;
    if (auto it = registry.options.find(key); it != registry.options.end()) {
        options = it->second;
    }
    if (!ShouldPersistEvent(event, severity, options)) {
        return true;
    }
    RotateEventLogIfDueLocked(registry, path, key, options);

    nlohmann::json payload = {
        {"schema", "svg_mb_control.event.v1"},
        {"event_time", event_time},
        {"mode", event.mode},
        {"event_type", event.event_type},
        {"severity", severity},
        {"error_code", error_code},
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

    // Serialize once so the line can be written in a single I/O call. With
    // FILE_APPEND_DATA (and no FILE_WRITE_DATA), NTFS atomically updates
    // the file pointer and writes the bytes, so concurrent appenders never
    // interleave NDJSON lines. Without this, two CLIs that emit events
    // (e.g. --write-once, --reconcile, plus the worker) could splice
    // payloads together since none of them are under the worker singleton.
    std::string line = payload.dump();
    line.push_back('\n');

    if (!AppendLineAtomic(path, line)) {
        return false;
    }

    NoteEventAppendedLocked(registry, path, key);
    return true;
}

bool AppendControlLoopEvent(const std::filesystem::path& runtime_home,
                            RuntimeLogEvent event,
                            const RuntimeArtifactNaming& naming) {
    if (event.mode.empty()) {
        event.mode = "control-loop";
    }
    return AppendRuntimeEvent(runtime_home, event, naming);
}

void LogOrchestratorEvent(const std::filesystem::path& runtime_home,
                          std::string_view mode,
                          std::string_view event_type,
                          std::string detail,
                          std::optional<std::uint32_t> channel,
                          std::optional<double> target_pct,
                          bool success) {
    RuntimeLogEvent event;
    event.mode = std::string(mode);
    event.event_type = std::string(event_type);
    event.detail = std::move(detail);
    event.channel = channel;
    event.target_pct = target_pct;
    event.success = success;
    AppendRuntimeEvent(runtime_home, event);
}

}  // namespace svg_mb_control
