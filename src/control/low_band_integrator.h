#pragma once

#include "control_policy.h"
#include "control_runtime_context.h"
#include "runtime_snapshot.h"

#include <chrono>
#include <cstdint>

namespace svg_mb_control {

// Advances the global low-band signal/debt and each channel's staged
// boost by one tick. Runs once per loop iteration before per-channel
// evaluation, so the boost fields it reads from `context.channels` for
// the primary-response veto reflect the previous tick — a one-tick lag
// is acceptable here (see body comment).
//
// `elapsed_ms` is the achieved tick interval; pass 0 for the first tick
// to fall back to the configured `poll_tick_ms`.
void UpdateLowBandState(ControlRuntimeContext& context,
                        const TempInputs& temp_inputs,
                        const RuntimeSnapshotIndex& runtime_index,
                        std::uint64_t elapsed_ms,
                        std::chrono::steady_clock::time_point now,
                        std::uint64_t tick_count);

}  // namespace svg_mb_control
