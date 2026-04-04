#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace svg_mb_control {

struct ControlConfig {
    std::filesystem::path source_path;
    std::filesystem::path bench_exe_path;
    std::filesystem::path snapshot_path;
    std::uint32_t poll_ms = 1000u;
};

std::filesystem::path GetEnvironmentPath(std::wstring_view name);
std::filesystem::path ResolveDefaultControlConfigPath();
ControlConfig LoadControlConfig(const std::filesystem::path& path);

}  // namespace svg_mb_control
