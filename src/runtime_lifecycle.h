#pragma once

#include <filesystem>

namespace svg_mb_control {

std::filesystem::path RuntimeStopRequestPath(
    const std::filesystem::path& runtime_home);

bool RuntimeStopRequested(const std::filesystem::path& runtime_home);

bool RequestRuntimeStop(const std::filesystem::path& runtime_home);

void ClearRuntimeStopRequest(const std::filesystem::path& runtime_home);

}  // namespace svg_mb_control
