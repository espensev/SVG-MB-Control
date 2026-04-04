#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace svg_mb_control {

struct BridgeProcessResult {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
};

struct JsonArtifactLaunchResult {
    BridgeProcessResult process;
    std::filesystem::path json_artifact_path;
};

std::filesystem::path ResolveDefaultBenchExecutablePath();

BridgeProcessResult RunBenchProcess(
    const std::wstring& bench_exe_path,
    const std::vector<std::wstring>& args,
    std::uint32_t timeout_ms);

JsonArtifactLaunchResult RunReadSnapshot(
    const std::wstring& bench_exe_path,
    std::uint32_t timeout_ms);

JsonArtifactLaunchResult RunLoggerService(
    const std::wstring& bench_exe_path,
    std::uint32_t duration_ms,
    std::uint32_t timeout_ms);

std::string LoadJsonObjectFile(
    const std::filesystem::path& json_path);

}  // namespace svg_mb_control
