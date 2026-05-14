#include "control_runtime_context.h"

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
    channels.reserve(loop.channels.size());
    for (const auto& channel_config : loop.channels) {
        ChannelState state;
        state.config = channel_config;
        channels.push_back(std::move(state));
    }
}

}  // namespace svg_mb_control
