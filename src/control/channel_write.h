#pragma once

#include "channel_evaluator.h"
#include "control_runtime_context.h"
#include "fan_writer.h"
#include "pending_writes.h"
#include "runtime_snapshot.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace svg_mb_control {

// Records the channel's baseline duty/mode the first time a fan snapshot
// is available, and emits a `control_loop.baseline_captured` event. A
// no-op once `channel.baseline_captured` is true or when no fan snapshot
// exists for the channel id.
void CaptureChannelBaselineIfAvailable(
    const ControlRuntimeContext& context,
    ChannelState& channel,
    const RuntimeSnapshotIndex& runtime_index,
    std::uint64_t tick_count);

// Emits per-channel sensor-failure / sensor-recovery events when the
// evaluator reports a transition. No-op when `evaluation.sensor_event`
// is `None`.
void AppendChannelSensorEvent(const ControlRuntimeContext& context,
                              const ChannelState& channel,
                              const ChannelEvaluation& evaluation,
                              std::uint64_t tick_count);

// Returns false (and sets the fatal_restore_* out-params) when a restore
// has timed out and the loop should abort; returns true otherwise. The
// returned bool follows the loop's existing convention from when this
// helper was inline in `RunUntilStopped`.
bool HandleExpiredHoldRestore(
    ControlRuntimeContext& context,
    ChannelState& channel,
    const ChannelTimingConfig& channel_timing,
    FanWriter& fan_writer,
    PendingWritesStore& pending_store,
    std::chrono::steady_clock::time_point now_steady,
    std::uint64_t tick_count,
    std::string& last_successful_restore_iso,
    bool& fatal_restore_timeout,
    std::uint32_t& fatal_restore_channel);

// Applies a single channel's setpoint subject to deadband, cooldown,
// baseline-captured, runtime-policy, and circuit-breaker gates. Performs
// the sidecar upsert before the fan write, and emits the appropriate
// `control_loop.write_*` / `control_loop.circuit_breaker_*` /
// `control_loop.authority_reasserted` events.
void TryApplyChannelSetpoint(
    ControlRuntimeContext& context,
    ChannelState& channel,
    const RuntimeSnapshotIndex& runtime_index,
    const ChannelEvaluation& evaluation,
    FanWriter& fan_writer,
    PendingWritesStoreInterface& pending_store,
    std::string_view eval_iso,
    std::chrono::steady_clock::time_point now_steady,
    std::uint64_t tick_count);

// FEAT-0019 (REQ-WRITEHOT-06): clears every channel's
// `consecutive_sidecar_persist_failures`. Called by the tick loop after a
// successful end-of-tick `Flush()` — a successful flush rewrites the whole
// sidecar, so every channel's recovery record is current and any prior
// persist-failure degradation (e.g. a failed activation that self-heals through
// the deferred write) is resolved. Pairs with the identity-gated reset in
// `TryApplyChannelSetpoint`, which clears only the channel it persisted.
void ClearSidecarPersistFailures(std::vector<ChannelState>& channels);

// FEAT-0019 (REQ-WRITEHOT-06): runs the end-of-tick sidecar `Flush()` and, IFF
// it actually persisted (Flush returned true — the whole sidecar was rewritten,
// so every channel's recovery record is now current), clears every channel's
// `consecutive_sidecar_persist_failures` via `ClearSidecarPersistFailures`.
// Returns whether the flush persisted. A no-op flush (nothing dirty) returns
// false and leaves the counters untouched — the clear MUST stay gated on the
// persist so a quiescent tick cannot falsely heal a channel whose record is not
// on disk. This is the composition the control tick runs each iteration;
// factored out so the persist-gated clear is unit-testable without a full
// `RunControlTick` harness. Propagates a `Flush()` filesystem exception.
bool FlushAndClearPersistFailures(PendingWritesStore& store,
                                  std::vector<ChannelState>& channels);

}  // namespace svg_mb_control
