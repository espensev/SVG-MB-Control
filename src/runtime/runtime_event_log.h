#pragma once

#include "runtime_paths.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace svg_mb_control {

struct RuntimeLogEvent {
    std::string event_time_iso;
    std::string mode;
    std::string event_type;
    std::string detail;
    std::optional<std::uint32_t> channel;
    std::optional<std::uint64_t> tick_count;
    std::optional<double> observed_temp_c;
    std::optional<double> setpoint_pct;
    std::optional<double> target_pct;
    std::optional<bool> success;
    std::string snapshot_time_iso;
    std::string log_csv_path;
    std::string event_log_path;
    std::optional<std::uint32_t> amd_sensor_count;
    std::optional<std::uint32_t> fan_count;
    std::optional<bool> gpu_available;
    std::optional<std::uint64_t> successful_polls;
    std::optional<std::uint64_t> skipped_polls;
    std::optional<bool> stale;
    std::optional<bool> telemetry_available;
    std::optional<bool> runtime_home_published;
    std::optional<bool> snapshot_mirror_configured;
    std::optional<bool> snapshot_mirror_published;
};

bool AppendRuntimeEvent(const std::filesystem::path& runtime_home,
                        const RuntimeLogEvent& event,
                        const RuntimeArtifactNaming& naming =
                            RuntimeArtifactNaming{});

// Number of non-empty lines in the event log at `event_log_path`. Cached
// per path; pass refresh_from_disk=true to re-read instead of using the
// cached value. Exposed for the CSV manifest writer's event_count field.
std::uint64_t CachedEventCount(const std::filesystem::path& event_log_path,
                               bool refresh_from_disk);

}  // namespace svg_mb_control
