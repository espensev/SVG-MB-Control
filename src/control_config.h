#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace svg_mb_control {

struct ControlConfig {
    std::filesystem::path source_path;
    std::uint32_t schema_version = 1u;
    std::string default_mode;
    std::filesystem::path snapshot_path;
    std::uint32_t poll_ms = 1000u;

    // Runtime layout fields.
    std::filesystem::path runtime_home_path;
    std::uint32_t staleness_threshold_ms = 0u;

    // Write-once fields.
    std::uint32_t write_channel = 0u;
    bool write_channel_set = false;
    double write_target_pct = 0.0;
    bool write_target_pct_set = false;
    std::uint32_t write_hold_ms = 0u;
    bool write_hold_ms_set = false;
    std::uint32_t baseline_freshness_ceiling_ms = 2000u;
    std::uint32_t restore_timeout_ms = 5000u;

    // Runtime policy path. When set, Control exports
    // SVG_MB_RUNTIME_POLICY=<resolved path> into its own process
    // environment and also uses the same file for local write gating.
    std::filesystem::path runtime_policy_path;
};

std::filesystem::path GetEnvironmentPath(std::wstring_view name);
std::filesystem::path ResolveDefaultControlConfigPath();
ControlConfig LoadControlConfig(const std::filesystem::path& path);

}  // namespace svg_mb_control
