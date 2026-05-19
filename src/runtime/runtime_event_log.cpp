#include "runtime_event_log.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace svg_mb_control {

namespace {

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

std::string EventCountKey(const std::filesystem::path& path) {
    return path.lexically_normal().string();
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
    if (!stream.good()) {
        return false;
    }
    NoteEventAppended(path);
    return true;
}

}  // namespace svg_mb_control
