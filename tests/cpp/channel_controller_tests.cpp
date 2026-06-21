// Tests for the FEAT-0003 control-law seam (slice F3-1): the IChannelController
// interface, its default factory, and the CurveOverlayController forward-wrap.
//
// The forwarding test is deliberately a wiring guard, not a math check. The
// CurveOverlayController forwards to the same EvaluateChannel the legacy
// per-channel loop called directly, so identical inputs must yield an identical
// ChannelEvaluation. It pins the new factory/interface contract
// (Kind()=="curve_overlay" by default; Evaluate forwards the full evaluation) so
// a later dispatch-on-controller_kind change (slice F3-2) cannot silently break
// the curve law's path. The end-to-end wiring through the index-based tick loop
// is covered by tests/test_control_loop.py.

#include "channel_controller.h"

#include "test_helpers.h"

#include "channel_evaluator.h"
#include "control_policy.h"
#include "control_runtime_context.h"
#include "runtime_snapshot.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace {

using namespace svg_mb_control;

// A representative channel that rides the curve on a present CPU temperature.
ChannelState MakeCurveChannel() {
    ChannelState channel;
    channel.config.channel = 0u;
    channel.config.temp_blend = TempBlend::CpuOnly;
    channel.config.curve = {{30.0, 20.0}, {70.0, 100.0}};
    return channel;
}

TempInputs MakeCpuInput(double cpu_c) {
    TempInputs in;
    in.cpu_c = cpu_c;
    in.cpu_available = true;
    in.cpu_label = "cpu";
    return in;
}

// REQ-PROFILE-03 (default selection): a channel with no controller discriminator
// resolves to the curve-overlay law.
void TestFactoryDefaultsToCurveOverlayKind() {
    ChannelControlConfig config;
    const std::unique_ptr<IChannelController> controller =
        CreateChannelController(config);
    ExpectTrue(controller != nullptr,
               "CreateChannelController returns a controller for a default config");
    if (controller) {
        ExpectEqual(std::string(controller->Kind()), "curve_overlay",
                    "the default controller is the curve-overlay law");
    }
}

// REQ-PROFILE-06 (law-agnostic seam): the curve-overlay controller forwards to
// EvaluateChannel, so identical inputs yield an identical ChannelEvaluation.
void TestCurveOverlayForwardsToEvaluateChannel() {
    ControlLoopConfig loop;
    RuntimeSnapshot empty_snapshot;
    RuntimeSnapshotIndex index;
    index.Rebuild(empty_snapshot);
    const auto now = std::chrono::steady_clock::now();
    const TempInputs in = MakeCpuInput(55.0);

    // Two identical channels: one through the legacy direct call, one through the
    // controller. EvaluateChannel mutates ChannelState, so they must not share.
    ChannelState direct_channel = MakeCurveChannel();
    ChannelState wrapped_channel = MakeCurveChannel();

    const ChannelEvaluation direct =
        EvaluateChannel(direct_channel, loop, in, index, now);

    const std::unique_ptr<IChannelController> controller =
        CreateChannelController(wrapped_channel.config);
    ExpectTrue(controller != nullptr,
               "controller constructed for the curve channel");
    if (!controller) {
        return;
    }
    const ChannelEvaluation wrapped =
        controller->Evaluate(wrapped_channel, loop, in, index, now);

    ExpectTrue(wrapped.has_setpoint == direct.has_setpoint,
               "forwarded has_setpoint matches the direct call");
    ExpectNear(wrapped.setpoint_pct, direct.setpoint_pct, 1e-12,
               "forwarded setpoint_pct matches the direct call");
    ExpectNear(wrapped.observed_temp_c, direct.observed_temp_c, 1e-12,
               "forwarded observed_temp_c matches the direct call");
    ExpectEqual(wrapped.response_source, direct.response_source,
                "forwarded response_source matches the direct call");
    ExpectTrue(wrapped.safety_override == direct.safety_override,
               "forwarded safety_override matches the direct call");
    ExpectTrue(wrapped.authority_reassert == direct.authority_reassert,
               "forwarded authority_reassert matches the direct call");
    // A representative curve input produces a real setpoint, proving the forward
    // actually ran the law rather than returning a default-constructed evaluation.
    ExpectTrue(direct.has_setpoint,
               "the representative curve input produces a setpoint (guards a "
               "hollow forward)");
}

// REQ-PROFILE-05 (per-channel construction): ControlRuntimeContext builds one
// controller per channel, index-aligned with `channels`, so the tick loop's
// controllers[i] / channels[i] pairing can never read out of bounds. Each
// controller defaults to the curve-overlay law.
void TestContextBuildsOneControllerPerChannelIndexAligned() {
    ControlLoopConfig loop;
    for (std::uint32_t ch = 0; ch < 3u; ++ch) {
        ChannelControlConfig config;
        config.channel = ch;
        config.temp_blend = TempBlend::CpuOnly;
        config.curve = {{30.0, 20.0}, {70.0, 100.0}};
        loop.channels.push_back(config);
    }

    const ControlRuntimeContext context(ControlConfig{}, loop,
                                         std::filesystem::path("."));

    ExpectTrue(context.controllers.size() == context.channels.size(),
               "one controller is built per channel");
    ExpectTrue(context.controllers.size() == 3u,
               "all three configured channels get a controller");
    for (std::size_t i = 0; i < context.controllers.size(); ++i) {
        ExpectTrue(context.controllers[i] != nullptr,
                   "each channel's controller is constructed");
        if (context.controllers[i]) {
            ExpectEqual(std::string(context.controllers[i]->Kind()),
                        "curve_overlay",
                        "each channel defaults to the curve-overlay law");
        }
    }
}

}  // namespace

int main() {
    TestFactoryDefaultsToCurveOverlayKind();
    TestCurveOverlayForwardsToEvaluateChannel();
    TestContextBuildsOneControllerPerChannelIndexAligned();

    if (g_failures != 0) {
        std::cerr << g_failures << " channel controller test failure(s)\n";
        return 1;
    }
    std::cout << "All channel controller tests passed\n";
    return 0;
}
