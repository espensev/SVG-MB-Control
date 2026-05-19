#pragma once

#include "control_runtime_context.h"
#include "runtime_csv_rows.h"

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
                            const std::string& log_manifest_path,
                            const std::string& event_log_path,
                            const std::string& last_successful_restore_iso = {});

std::vector<RuntimeControlChannelLogState> BuildChannelLogStates(
    const std::vector<ChannelState>& channels);

// Fill-in-place variant for the steady-state control loop: reuses `out`'s
// vector storage and per-element string capacity across ticks (resize +
// field assignment) instead of allocating a fresh vector every tick.
void BuildChannelLogStates(
    const std::vector<ChannelState>& channels,
    std::vector<RuntimeControlChannelLogState>& out);

}  // namespace svg_mb_control
