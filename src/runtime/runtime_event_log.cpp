#include "runtime_event_log.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
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

struct EventCountCache {
    std::mutex mutex;
    std::unordered_map<std::string, std::uint64_t> counts;
};

EventCountCache& RuntimeEventCountCache() {
    static EventCountCache cache;
    return cache;
}

void NoteEventAppended(const std::filesystem::path& path) {
    EventCountCache& cache = RuntimeEventCountCache();
    const std::string key = EventCountKey(path);
    std::lock_guard<std::mutex> lock(cache.mutex);
    auto existing = cache.counts.find(key);
    if (existing == cache.counts.end()) {
        cache.counts.emplace(key, CountNonEmptyLines(path));
        return;
    }
    ++existing->second;
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

}  // namespace

std::uint64_t CachedEventCount(const std::filesystem::path& path,
                               bool refresh_from_disk) {
    EventCountCache& cache = RuntimeEventCountCache();
    const std::string key = EventCountKey(path);
    std::lock_guard<std::mutex> lock(cache.mutex);
    const auto existing = cache.counts.find(key);
    if (!refresh_from_disk && existing != cache.counts.end()) {
        return existing->second;
    }

    const std::uint64_t count = CountNonEmptyLines(path);
    cache.counts[key] = count;
    return count;
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
        ? FormatRuntimeLocalIso8601(std::chrono::system_clock::now())
        : event.event_time_iso;
    const std::string severity = InferSeverity(event);
    const std::string error_code = InferErrorCode(event, severity);

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
    if (!ok || written != line.size()) {
        return false;
    }
#else
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    if (!stream.is_open()) {
        return false;
    }
    stream << line;
    stream.flush();
    if (!stream.good()) {
        return false;
    }
#endif

    NoteEventAppended(path);
    return true;
}

}  // namespace svg_mb_control
