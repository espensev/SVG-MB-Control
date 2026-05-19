#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace svg_mb_control {

struct RuntimeArtifactNaming {
    std::string archive_prefix = "svg_mb_control";
    std::string latest_csv_name = "svg_mb_control_output.csv";
    std::string latest_events_name = "svg_mb_control_events.jsonl";
    std::string latest_manifest_name = "svg_mb_control_manifest.json";
};

std::string FormatRuntimeLocalIso8601(
    std::chrono::system_clock::time_point tp);

std::filesystem::path ResolveRuntimeLogsDir(
    const std::filesystem::path& runtime_home);
std::filesystem::path ResolveRuntimeArchiveDir(
    const std::filesystem::path& runtime_home);
std::filesystem::path ResolveRuntimeLogMirrorPath(
    const std::filesystem::path& runtime_home);
std::filesystem::path ResolveRuntimeLogMirrorPath(
    const std::filesystem::path& runtime_home,
    const RuntimeArtifactNaming& naming);
std::filesystem::path ResolveRuntimeEventLogPath(
    const std::filesystem::path& runtime_home);
std::filesystem::path ResolveRuntimeEventLogPath(
    const std::filesystem::path& runtime_home,
    const RuntimeArtifactNaming& naming);
std::filesystem::path ResolveRuntimeLogManifestPath(
    const std::filesystem::path& runtime_home);
std::filesystem::path ResolveRuntimeLogManifestPath(
    const std::filesystem::path& runtime_home,
    const RuntimeArtifactNaming& naming);

}  // namespace svg_mb_control
