#include "channel_controller.h"

#include "pid_controller.h"

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
    // FEAT-0003 (REQ-PROFILE-03): dispatch on the resolved controller kind. An
    // absent `controller` key parsed to CurveOverlay, so existing configs keep
    // today's curve law. A PidController is built shadow/dry-run unless its
    // pid.allow_live evidence + slew-cap preconditions hold (decision D6).
    if (config.controller_kind == ControllerKind::Pid) {
        return std::make_unique<PidController>(config);
    }
    return std::make_unique<CurveOverlayController>();
}

}  // namespace svg_mb_control
