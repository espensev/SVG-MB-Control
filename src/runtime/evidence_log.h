#pragma once

#include "control_config.h"
#include "runtime_artifacts.h"

#include <atomic>
#include <filesystem>

namespace svg_mb_control {

RuntimeArtifactNaming EvidenceLogArtifactNaming();

int RunEvidenceLog(const ControlConfig& config,
                   const std::filesystem::path& runtime_home,
                   const std::atomic<bool>& stop_requested);

}  // namespace svg_mb_control
