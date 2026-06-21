#include "channel_controller.h"

namespace svg_mb_control {

ChannelEvaluation CurveOverlayController::Evaluate(
    ChannelState& channel,
    const ControlLoopConfig& loop,
    const TempInputs& temp_inputs,
    const RuntimeSnapshotIndex& runtime_index,
    std::chrono::steady_clock::time_point now) {
    return EvaluateChannel(channel, loop, temp_inputs, runtime_index, now);
}

void CurveOverlayController::Reset() {}

std::string_view CurveOverlayController::Kind() const { return "curve_overlay"; }

std::unique_ptr<IChannelController> CreateChannelController(
    const ChannelControlConfig& config) {
    // Slice F3-1: every channel uses the curve-overlay law. Slice F3-2 will
    // dispatch on the controller discriminator parsed from config.
    (void)config;
    return std::make_unique<CurveOverlayController>();
}

}  // namespace svg_mb_control
