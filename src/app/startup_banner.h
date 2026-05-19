#pragma once

#include "control_config.h"
#include "control_loop.h"
#include "runtime_write_policy.h"

#include <filesystem>

namespace svg_mb_control {

void PrintControlLoopStartup(const ControlConfig& config,
                             const ControlLoopConfig& loop_config,
                             const std::filesystem::path& runtime_home,
                             const RuntimeWritePolicy& policy);

void PrintReadLoopStartup(const ControlConfig& config,
                          const std::filesystem::path& runtime_home,
                          const RuntimeWritePolicy& policy);

void PrintEvidenceLogStartup(const ControlConfig& config,
                             const std::filesystem::path& runtime_home,
                             const RuntimeWritePolicy& policy);

}  // namespace svg_mb_control
