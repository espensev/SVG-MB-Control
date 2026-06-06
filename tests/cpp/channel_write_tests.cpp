// Tests for the write-failure circuit-breaker gate in TryApplyChannelSetpoint.
//
// Covers recovery-gap remediation 3
// (docs/discovery-recovery-gap-audit-2026-06-04.md): a sensor-safe
// (thermal-safety) command must reach ApplyDuty even when the breaker is open,
// while a normal command must still be suppressed by an open breaker. Both legs
// run in the same breaker-open state so the test proves the gate condition
// `circuit_breaker_open && !safety_override` is wired as intended.
//
// It also covers the other half of the seam: that EvaluateChannel actually sets
// safety_override when a channel drops into sensor-safe mode, so the two changed
// files are proven to connect end to end.

#include "channel_evaluator.h"
#include "channel_write.h"
#include "control_policy.h"
#include "control_runtime_context.h"
#include "fan_writer.h"
#include "pending_writes.h"
#include "runtime_snapshot.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

int g_failures = 0;

void ExpectTrue(bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// Records every ApplyDuty call and returns a configurable result. All other
// FanWriter methods are inert defaults — the write-gate path only calls
// ApplyDuty (and, on the restore path, RestoreSavedState, which these tests do
// not exercise).
class RecordingFanWriter : public svg_mb_control::FanWriter {
 public:
    struct ApplyCall {
        std::uint32_t channel;
        double duty_pct;
    };

    std::vector<ApplyCall> apply_calls;
    svg_mb_control::FanWriteResult apply_result{};  // ok by default

    svg_mb_control::FanReadResult ReadChannelState(std::uint32_t) override {
        return {};
    }
    void ReadAllChannels(svg_mb_control::FanScanResult& out) override {
        out = svg_mb_control::FanScanResult{};
    }
    svg_mb_control::FanTachEvidenceScanResult ReadFanTachEvidence() override {
        return {};
    }
    svg_mb_control::SioVoltageScanResult ReadVoltages() override { return {}; }
    svg_mb_control::SioTemperatureScanResult ReadSioTemperatures() override {
        return {};
    }
    svg_mb_control::FanWriteResult ApplyDuty(std::uint32_t channel,
                                             double duty_pct) override {
        apply_calls.push_back(ApplyCall{channel, duty_pct});
        return apply_result;
    }
    svg_mb_control::FanWriteResult RestoreSavedState(std::uint32_t,
                                                     std::uint8_t, std::uint8_t,
                                                     std::uint32_t) override {
        return {};
    }
    std::string BackendLabel() const override { return "recording"; }
};

// A fresh, empty runtime-home directory for the pending-writes sidecar and the
// control-loop event log. Removed and recreated so each test starts clean.
std::filesystem::path MakeTempHome(const char* name) {
    std::filesystem::path home =
        std::filesystem::temp_directory_path() /
        (std::string("svg_mb_control_channel_write_tests_") + name);
    std::error_code ec;
    std::filesystem::remove_all(home, ec);
    std::filesystem::create_directories(home, ec);
    return home;
}

// Builds a channel that passes every write gate except the breaker: baseline
// captured, and last_issued_pct left NaN so first_write skips the deadband and
// cooldown gates. The breaker state is supplied by the caller.
svg_mb_control::ChannelState MakeReadyChannel(bool breaker_open) {
    svg_mb_control::ChannelState channel;
    channel.config.channel = 2u;
    channel.baseline_captured = true;
    channel.baseline_duty_raw = 10u;
    channel.baseline_mode_raw = 1u;
    channel.circuit_breaker_open = breaker_open;
    channel.consecutive_write_failures = breaker_open ? 5u : 0u;
    return channel;
}

svg_mb_control::ChannelEvaluation MakeEvaluation(double setpoint_pct,
                                                 bool safety_override) {
    svg_mb_control::ChannelEvaluation eval;
    eval.has_setpoint = true;
    eval.setpoint_pct = setpoint_pct;
    eval.response_source = safety_override ? "sensor_safe_mode" : "primary_curve";
    eval.safety_override = safety_override;
    return eval;
}

void RunWrite(svg_mb_control::ControlRuntimeContext& context,
              svg_mb_control::ChannelState& channel,
              const svg_mb_control::ChannelEvaluation& eval,
              RecordingFanWriter& writer,
              svg_mb_control::PendingWritesStore& store) {
    svg_mb_control::RuntimeSnapshot empty_snapshot;
    svg_mb_control::RuntimeSnapshotIndex index;
    index.Rebuild(empty_snapshot);  // FindFanChannel -> nullptr -> writes allowed
    svg_mb_control::TryApplyChannelSetpoint(
        context, channel, index, eval, writer, store, "2026-06-06T00:00:00",
        std::chrono::steady_clock::now(), /*tick_count=*/1u);
}

svg_mb_control::ControlRuntimeContext MakeContext(
    const std::filesystem::path& home) {
    return svg_mb_control::ControlRuntimeContext(
        svg_mb_control::ControlConfig{}, svg_mb_control::ControlLoopConfig{},
        home);
}

// Leg 1: a sensor-safe command reaches ApplyDuty past an open breaker, and a
// successful safety write closes the breaker (the path recovered).
void TestSensorSafeBypassesOpenBreaker() {
    const std::filesystem::path home = MakeTempHome("safe_bypass");
    svg_mb_control::ControlRuntimeContext context = MakeContext(home);
    svg_mb_control::ChannelState channel = MakeReadyChannel(/*breaker_open=*/true);
    svg_mb_control::PendingWritesStore store(home);
    RecordingFanWriter writer;

    const svg_mb_control::ChannelEvaluation eval =
        MakeEvaluation(100.0, /*safety_override=*/true);
    RunWrite(context, channel, eval, writer, store);

    ExpectTrue(writer.apply_calls.size() == 1u,
               "sensor-safe command reaches ApplyDuty past an open breaker");
    if (writer.apply_calls.size() == 1u) {
        ExpectTrue(writer.apply_calls.front().channel == 2u,
                   "bypassed safety write targets the channel");
        ExpectTrue(writer.apply_calls.front().duty_pct == 100.0,
                   "bypassed safety write carries the 100% safe-mode setpoint");
    }
    ExpectTrue(!channel.circuit_breaker_open,
               "successful safety write closes the breaker");

    std::error_code ec;
    std::filesystem::remove_all(home, ec);
}

// Leg 2 (same open-breaker state): a normal command is still suppressed, and
// the breaker stays open. This proves the fix did not let normal writes bypass.
void TestNormalWriteSuppressedByOpenBreaker() {
    const std::filesystem::path home = MakeTempHome("normal_suppressed");
    svg_mb_control::ControlRuntimeContext context = MakeContext(home);
    svg_mb_control::ChannelState channel = MakeReadyChannel(/*breaker_open=*/true);
    svg_mb_control::PendingWritesStore store(home);
    RecordingFanWriter writer;

    const svg_mb_control::ChannelEvaluation eval =
        MakeEvaluation(60.0, /*safety_override=*/false);
    RunWrite(context, channel, eval, writer, store);

    ExpectTrue(writer.apply_calls.empty(),
               "normal command is suppressed by an open breaker");
    ExpectTrue(channel.circuit_breaker_open,
               "suppressed normal write leaves the breaker open");

    std::error_code ec;
    std::filesystem::remove_all(home, ec);
}

// Positive control: with the breaker closed, a normal command reaches
// ApplyDuty. Proves the harness can reach the hardware, so Leg 2's "no call"
// is the breaker, not a broken setup.
void TestNormalWriteAppliesWhenBreakerClosed() {
    const std::filesystem::path home = MakeTempHome("normal_applies");
    svg_mb_control::ControlRuntimeContext context = MakeContext(home);
    svg_mb_control::ChannelState channel =
        MakeReadyChannel(/*breaker_open=*/false);
    svg_mb_control::PendingWritesStore store(home);
    RecordingFanWriter writer;

    const svg_mb_control::ChannelEvaluation eval =
        MakeEvaluation(60.0, /*safety_override=*/false);
    RunWrite(context, channel, eval, writer, store);

    ExpectTrue(writer.apply_calls.size() == 1u,
               "normal command reaches ApplyDuty when the breaker is closed");

    std::error_code ec;
    std::filesystem::remove_all(home, ec);
}

// A bypassed safety write that fails reaches the hardware (loud) and leaves the
// breaker open with the failure counter advanced — failure bookkeeping intact.
void TestSafetyWriteFailureKeepsBreakerOpen() {
    const std::filesystem::path home = MakeTempHome("safe_failure");
    svg_mb_control::ControlRuntimeContext context = MakeContext(home);
    svg_mb_control::ChannelState channel = MakeReadyChannel(/*breaker_open=*/true);
    svg_mb_control::PendingWritesStore store(home);
    RecordingFanWriter writer;
    writer.apply_result.error = svg_mb_control::FanWriteError::kWriteFailed;
    writer.apply_result.detail = "simulated write failure";

    const svg_mb_control::ChannelEvaluation eval =
        MakeEvaluation(100.0, /*safety_override=*/true);
    RunWrite(context, channel, eval, writer, store);

    ExpectTrue(writer.apply_calls.size() == 1u,
               "failed safety write still reaches the hardware (fails loud)");
    ExpectTrue(channel.circuit_breaker_open,
               "failed safety write leaves the breaker open");
    ExpectTrue(channel.consecutive_write_failures == 6u,
               "failed safety write advances the failure counter");

    std::error_code ec;
    std::filesystem::remove_all(home, ec);
}

// The other half of the seam: EvaluateChannel sets safety_override once a
// channel falls into sensor-safe mode (primary sensor unavailable past the
// failure threshold). EvaluateChannel is pure, so no context/temp dir needed.
void TestEvaluatorSetsSafetyOverrideOnSensorFailure() {
    using namespace svg_mb_control;
    ControlLoopConfig loop;
    ChannelState channel;
    channel.config.channel = 2u;
    channel.config.temp_blend = TempBlend::CpuOnly;
    channel.config.curve = {{30.0, 20.0}, {70.0, 100.0}};

    RuntimeSnapshot empty_snapshot;
    RuntimeSnapshotIndex index;
    index.Rebuild(empty_snapshot);
    const auto now = std::chrono::steady_clock::now();

    TempInputs no_input;  // cpu and gpu both unavailable -> primary missing

    // Below kMaxConsecutiveSensorFailures (3): no safe mode, no override.
    const ChannelEvaluation e1 =
        EvaluateChannel(channel, loop, no_input, index, now);
    ExpectTrue(!e1.safety_override,
               "no safety_override before the sensor-failure threshold (1)");
    const ChannelEvaluation e2 =
        EvaluateChannel(channel, loop, no_input, index, now);
    ExpectTrue(!e2.safety_override,
               "no safety_override before the sensor-failure threshold (2)");

    // 3rd consecutive miss crosses the threshold -> sensor-safe command.
    const ChannelEvaluation e3 =
        EvaluateChannel(channel, loop, no_input, index, now);
    ExpectTrue(e3.safety_override,
               "EvaluateChannel sets safety_override in sensor-safe mode");
    ExpectTrue(e3.response_source == "sensor_safe_mode",
               "sensor-safe command carries response_source sensor_safe_mode");
    ExpectTrue(e3.has_setpoint, "sensor-safe command produces a setpoint");

    // A present primary temperature clears safe mode and the override.
    TempInputs with_temp;
    with_temp.cpu_c = 55.0;
    with_temp.cpu_available = true;
    const ChannelEvaluation e4 =
        EvaluateChannel(channel, loop, with_temp, index, now);
    ExpectTrue(!e4.safety_override,
               "safety_override clears when a primary temperature returns");
}

}  // namespace

int main() {
    TestSensorSafeBypassesOpenBreaker();
    TestNormalWriteSuppressedByOpenBreaker();
    TestNormalWriteAppliesWhenBreakerClosed();
    TestSafetyWriteFailureKeepsBreakerOpen();
    TestEvaluatorSetsSafetyOverrideOnSensorFailure();

    if (g_failures != 0) {
        std::cerr << g_failures << " channel write test failure(s)\n";
        return 1;
    }
    return 0;
}
