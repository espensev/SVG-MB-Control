#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace svg_mb_control {

struct ControlConfig {
    std::filesystem::path source_path;
    std::uint32_t schema_version = 1u;
    std::filesystem::path bench_exe_path;
    std::filesystem::path snapshot_path;
    std::uint32_t poll_ms = 1000u;

    // Phase 1 fields (schema_version >= 2).
    std::filesystem::path runtime_home_path;
    std::uint32_t staleness_threshold_ms = 0u;
    std::uint32_t snapshot_read_retry_count = 3u;
    std::uint32_t snapshot_read_retry_backoff_ms = 10u;
    std::uint32_t child_restart_budget = 3u;
    std::uint32_t child_restart_backoff_ms = 1000u;
    std::uint32_t logger_service_duration_ms = 86400000u;
};

std::filesystem::path GetEnvironmentPath(std::wstring_view name);
std::filesystem::path ResolveDefaultControlConfigPath();
ControlConfig LoadControlConfig(const std::filesystem::path& path);

}  // namespace svg_mb_control
