#pragma once

#include "control_runtime_context.h"
#include "runtime_csv_rows.h"

#include <cstdint>
#include <vector>

namespace svg_mb_control {

// Fill-in-place builder for the CSV-row log states. Reuses `out`'s vector
// storage and per-element string capacity across ticks (resize + field
// assignment) instead of allocating a fresh vector every tick. The
// control_runtime.json writer lives in runtime_status.h.
void BuildChannelLogStates(
    const std::vector<ChannelState>& channels,
    std::vector<RuntimeControlChannelLogState>& out);

}  // namespace svg_mb_control
