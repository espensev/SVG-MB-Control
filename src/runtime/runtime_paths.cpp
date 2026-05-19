#include "runtime_paths.h"

#include <array>
#include <ctime>
#include <string>

namespace svg_mb_control {

std::string FormatRuntimeLocalIso8601(
    std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
    if (localtime_s(&local, &tt) != 0) {
        return {};
    }
    std::array<char, 32> buffer{};
    const std::size_t written = std::strftime(buffer.data(), buffer.size(),
                                              "%Y-%m-%dT%H:%M:%S", &local);
    return written > 0u ? std::string(buffer.data(), written) : std::string();
}

std::filesystem::path ResolveRuntimeLogsDir(
    const std::filesystem::path& runtime_home) {
    return runtime_home / "logs";
}

std::filesystem::path ResolveRuntimeArchiveDir(
    const std::filesystem::path& runtime_home) {
    return ResolveRuntimeLogsDir(runtime_home) / "archive";
}

std::filesystem::path ResolveRuntimeLogMirrorPath(
    const std::filesystem::path& runtime_home) {
    return ResolveRuntimeLogMirrorPath(runtime_home, RuntimeArtifactNaming{});
}

std::filesystem::path ResolveRuntimeLogMirrorPath(
    const std::filesystem::path& runtime_home,
    const RuntimeArtifactNaming& naming) {
    return ResolveRuntimeLogsDir(runtime_home) / naming.latest_csv_name;
}

std::filesystem::path ResolveRuntimeEventLogPath(
    const std::filesystem::path& runtime_home) {
    return ResolveRuntimeEventLogPath(runtime_home, RuntimeArtifactNaming{});
}

std::filesystem::path ResolveRuntimeEventLogPath(
    const std::filesystem::path& runtime_home,
    const RuntimeArtifactNaming& naming) {
    return ResolveRuntimeLogsDir(runtime_home) / naming.latest_events_name;
}

std::filesystem::path ResolveRuntimeLogManifestPath(
    const std::filesystem::path& runtime_home) {
    return ResolveRuntimeLogManifestPath(runtime_home, RuntimeArtifactNaming{});
}

std::filesystem::path ResolveRuntimeLogManifestPath(
    const std::filesystem::path& runtime_home,
    const RuntimeArtifactNaming& naming) {
    return ResolveRuntimeLogsDir(runtime_home) / naming.latest_manifest_name;
}

}  // namespace svg_mb_control
