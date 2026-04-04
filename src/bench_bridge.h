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

struct ReadSnapshotLaunchResult {
    BridgeProcessResult process;
    std::filesystem::path snapshot_archive_path;
};

std::filesystem::path ResolveDefaultBenchExecutablePath();

BridgeProcessResult RunBenchProcess(
    const std::wstring& bench_exe_path,
    const std::vector<std::wstring>& args,
    std::uint32_t timeout_ms);

ReadSnapshotLaunchResult RunReadSnapshot(
    const std::wstring& bench_exe_path,
    std::uint32_t timeout_ms);

std::string LoadSnapshotJson(
    const std::filesystem::path& snapshot_archive_path);

}  // namespace svg_mb_control
