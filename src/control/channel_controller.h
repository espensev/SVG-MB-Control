#pragma once

#include "channel_evaluator.h"
#include "control_policy.h"
#include "runtime_snapshot.h"

#include <chrono>
#include <memory>
#include <string_view>

namespace svg_mb_control {

// FEAT-0003: per-channel control-law seam. Each controlled channel owns one
// IChannelController. The curve-overlay law (today's only law) is wrapped by
// CurveOverlayController, which forwards to EvaluateChannel; a future PID law is
// a second implementation selected per channel from config (REQ-PROFILE-03/06).
// The law is restart-selected and fixed for a worker's lifetime
// (docs/multiprofile-restart-switch-decision-2026-06-20.md D-MPROFILE-6), so the
// interface needs no in-process re-selection.
class IChannelController {
 public:
    virtual ~IChannelController() = default;

    // Evaluate one tick for `channel`, mutating its per-tick state, and return
    // the fully clamped + rate-limited ChannelEvaluation the law-agnostic write
    // path consumes (REQ-PROFILE-06). setpoint_pct is already in [0, 100] and
    // slew-limited before it leaves this call.
    virtual ChannelEvaluation Evaluate(
        ChannelState& channel,
        const ControlLoopConfig& loop,
        const TempInputs& temp_inputs,
        const RuntimeSnapshotIndex& runtime_index,
        std::chrono::steady_clock::time_point now) = 0;

    // Reset controller-internal state (e.g. a PID integral/derivative). The
    // curve-overlay law keeps all of its state in ChannelState, so its Reset is
    // a no-op.
    virtual void Reset() = 0;

    // Stable identifier of the control law, surfaced in reporting
    // (REQ-PROFILE-08).
    virtual std::string_view Kind() const = 0;
};

// The curve-overlay control law: forwards to EvaluateChannel so the existing
// curve + boost pipeline is unchanged behind the seam.
class CurveOverlayController final : public IChannelController {
 public:
    ChannelEvaluation Evaluate(
        ChannelState& channel,
        const ControlLoopConfig& loop,
        const TempInputs& temp_inputs,
        const RuntimeSnapshotIndex& runtime_index,
        std::chrono::steady_clock::time_point now) override;
    void Reset() override;
    std::string_view Kind() const override;
};

// Construct the controller for `config`. Slice F3-1 returns CurveOverlayController
// for every channel; slice F3-2 will dispatch on the controller discriminator
// parsed from config.
std::unique_ptr<IChannelController> CreateChannelController(
    const ChannelControlConfig& config);

}  // namespace svg_mb_control
