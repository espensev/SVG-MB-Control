#pragma once

#include "control_runtime_context.h"
#include "runtime_logging.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace svg_mb_control {

bool WriteControlLoopStatus(const std::filesystem::path& runtime_home,
                            const std::string& mode_label,
                            const std::string& status,
                            const std::string& status_detail,
                            std::uint64_t tick_count,
                            const std::string& last_evaluation_iso,
                            const RuntimeControlLoopTimingState& timing,
                            const std::vector<ChannelState>& channels,
                            const std::string& log_csv_path,
                            const std::string& event_log_path);

std::vector<RuntimeControlChannelLogState> BuildChannelLogStates(
    const std::vector<ChannelState>& channels);

}  // namespace svg_mb_control
