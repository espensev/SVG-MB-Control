#include "runtime_paths.h"

namespace svg_mb_control {

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

std::filesystem::path RuntimeLoggingHealthPath(
    const std::filesystem::path& runtime_home) {
    return runtime_home / "logging_health.json";
}

std::filesystem::path RuntimeStatusPath(
    const std::filesystem::path& runtime_home) {
    return runtime_home / "control_runtime.json";
}

}  // namespace svg_mb_control
