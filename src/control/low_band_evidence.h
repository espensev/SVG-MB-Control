#pragma once

#include "control_runtime_context.h"

#include <cstdint>

namespace svg_mb_control {

// True when the channel has a configured low-band stage with valid debt
// threshold and a positive max boost. Shared by the control loop's low-band
// state update and the evidence serializer below.
bool LowBandChannelConfigured(const ChannelState& channel);

// Serializes the current low-band runtime/aggregation state to
// <runtime_home>/low_band_evidence.json. Pure output: reads ControlRuntimeContext,
// writes one file in-process. On write failure, appends a control-loop runtime
// event. Throttling of how often this is called is the caller's concern.
void WriteLowBandEvidenceFile(const ControlRuntimeContext& context,
                              std::uint64_t tick_count);

}  // namespace svg_mb_control
