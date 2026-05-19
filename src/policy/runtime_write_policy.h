#pragma once

#include "control_config.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace svg_mb_control {

struct RuntimeWritePolicy {
    bool present = false;
    std::filesystem::path source_path;
    bool writes_enabled = true;
    bool restore_on_exit = true;
    std::vector<std::uint32_t> blocked_channels;
};

RuntimeWritePolicy LoadRuntimeWritePolicy(
    const std::filesystem::path& path);

RuntimeWritePolicy ResolveRuntimeWritePolicy(
    const ControlConfig* config);

bool RuntimeWritePolicyBlocksChannel(const RuntimeWritePolicy& policy,
                                     std::uint32_t channel);

}  // namespace svg_mb_control
