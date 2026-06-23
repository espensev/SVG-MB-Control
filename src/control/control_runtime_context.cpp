#include "control_runtime_context.h"

#include "channel_controller.h"

#include <string>
#include <utility>

namespace svg_mb_control {

ControlRuntimeContext::ControlRuntimeContext(
    ControlConfig base_config,
    ControlLoopConfig loop_config,
    std::filesystem::path runtime_home_path)
    : base(std::move(base_config)),
      loop(std::move(loop_config)),
      runtime_home(std::move(runtime_home_path)),
      runtime_policy(ResolveRuntimeWritePolicy(&base)) {
    low_band.enabled = loop.low_band.enabled;
    channels.reserve(loop.channels.size());
    controllers.reserve(loop.channels.size());
    // Build the controller and the channel state for each configured channel in
    // one loop so `controllers` and `channels` stay index-aligned by construction
    // (the tick loop indexes both with the same i).
    for (const auto& channel_config : loop.channels) {
        ChannelState state;
        state.config = channel_config;
        channels.push_back(std::move(state));
        controllers.push_back(CreateChannelController(channel_config));
        // Attribute the channel to the law that will drive it (REQ-PROFILE-08).
        // Set once here from the controller's own Kind(), the single source of
        // truth, so reporting cannot drift from the dispatched law.
        channels.back().controller_kind =
            std::string(controllers.back()->Kind());
    }
}

ControlRuntimeContext::~ControlRuntimeContext() = default;

}  // namespace svg_mb_control
