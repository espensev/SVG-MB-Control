#pragma once

#include "runtime_util.h"  // FormatLocalIso8601 — re-exported for legacy
                           // callers that previously used
                           // FormatRuntimeLocalIso8601 from this header.

#include <filesystem>
#include <string>

namespace svg_mb_control {

struct RuntimeArtifactNaming {
    std::string archive_prefix = "svg_mb_control";
    std::string latest_csv_name = "svg_mb_control_output.csv";
    std::string latest_events_name = "svg_mb_control_events.jsonl";
    std::string latest_manifest_name = "svg_mb_control_manifest.json";
};

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

// runtime_home / "control_runtime.json" — the path of the dual-schema
// (control-loop / read-loop) runtime status sidecar.
std::filesystem::path RuntimeStatusPath(
    const std::filesystem::path& runtime_home);

}  // namespace svg_mb_control
