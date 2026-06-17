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

#include "test_helpers.h"

#include "channel_write.h"
#include "control_policy.h"
#include "control_runtime_context.h"
#include "fan_writer.h"
#include "pending_writes.h"
#include "runtime_snapshot.h"
#include "runtime_status.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

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

// A pending-writes store whose Upsert always throws, simulating a persistent
// pending_writes.json fault (lock, ACL, disk error) for FEAT-0010. QueueRemove
// is inert; only Upsert/QueueRemove are reached through the injected reference.
class ThrowingPendingWritesStore
    : public svg_mb_control::PendingWritesStoreInterface {
 public:
    int upsert_calls = 0;
    void Upsert(const svg_mb_control::PendingWriteEntry&) override {
        ++upsert_calls;
        throw std::runtime_error("simulated sidecar persist failure");
    }
    void QueueRemove(std::uint32_t) override {}
};

// Like ThrowingPendingWritesStore, but the exception message carries invalid
// UTF-8 bytes so the failure-path event's JSON serialization (nlohmann dump())
// throws — used to exercise the "event logging must not veto actuation"
// guarantee (FEAT-0010 REQ-WRITESAFE-06).
class Utf8ThrowingPendingWritesStore
    : public svg_mb_control::PendingWritesStoreInterface {
 public:
    void Upsert(const svg_mb_control::PendingWriteEntry&) override {
        throw std::runtime_error(
            std::string("sidecar persist failed \xFF\xFE invalid-utf8"));
    }
    void QueueRemove(std::uint32_t) override {}
};

// Returns true if any file under <home>/logs contains `needle`. Asserts runtime
// event emission without depending on the exact event-log filename.
bool EventLogContains(const std::filesystem::path& home,
                      const std::string& needle) {
    const std::filesystem::path logs = home / "logs";
    std::error_code ec;
    if (!std::filesystem::exists(logs, ec)) {
        return false;
    }
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(logs, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream file(entry.path(), std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
        if (content.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// A fresh, empty runtime-home directory for the pending-writes sidecar and the
// control-loop event log. The name carries a per-process salt (UniqueTempSuffix)
// so concurrent test processes do not collide; removed and recreated so each
// test starts clean.
std::filesystem::path MakeTempHome(const char* name) {
    std::filesystem::path home =
        std::filesystem::temp_directory_path() /
        (std::string("svg_mb_control_channel_write_tests_") + name + "_" +
         UniqueTempSuffix());
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
              svg_mb_control::PendingWritesStoreInterface& store) {
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

// FEAT-0010 REQ-WRITESAFE-01: a failure to persist the pending-write sidecar
// must not stop the computed fan duty from being applied.
void TestSidecarPersistFailureStillActuates() {
    const std::filesystem::path home = MakeTempHome("persist_fail_actuates");
    svg_mb_control::ControlRuntimeContext context = MakeContext(home);
    svg_mb_control::ChannelState channel =
        MakeReadyChannel(/*breaker_open=*/false);
    ThrowingPendingWritesStore store;
    RecordingFanWriter writer;

    const svg_mb_control::ChannelEvaluation eval =
        MakeEvaluation(60.0, /*safety_override=*/false);
    RunWrite(context, channel, eval, writer, store);

    ExpectTrue(writer.apply_calls.size() == 1u,
               "a sidecar persist failure does not veto the fan write");
    if (writer.apply_calls.size() == 1u) {
        ExpectTrue(writer.apply_calls.front().duty_pct == 60.0,
                   "the computed setpoint is applied despite the persist fault");
    }

    std::error_code ec;
    std::filesystem::remove_all(home, ec);
}

// FEAT-0010 REQ-WRITESAFE-02: the sensor-safe (safety_override) command must
// reach the actuator regardless of sidecar persist success — even with the
// write-failure breaker open.
void TestSafetyOverrideActuatesDespiteSidecarPersistFailure() {
    const std::filesystem::path home = MakeTempHome("persist_fail_safe");
    svg_mb_control::ControlRuntimeContext context = MakeContext(home);
    svg_mb_control::ChannelState channel = MakeReadyChannel(/*breaker_open=*/true);
    ThrowingPendingWritesStore store;
    RecordingFanWriter writer;

    const svg_mb_control::ChannelEvaluation eval =
        MakeEvaluation(100.0, /*safety_override=*/true);
    RunWrite(context, channel, eval, writer, store);

    ExpectTrue(writer.apply_calls.size() == 1u,
               "sensor-safe command actuates despite a sidecar persist failure");
    if (writer.apply_calls.size() == 1u) {
        ExpectTrue(writer.apply_calls.front().duty_pct == 100.0,
                   "the 100% safe-mode command reaches the actuator");
    }

    std::error_code ec;
    std::filesystem::remove_all(home, ec);
}

// FEAT-0010 REQ-WRITESAFE-03: a persist failure followed by a successful
// actuation increments the additive per-channel persist-failure counter and
// must NOT increment consecutive_write_failures or open the write-failure
// breaker (the actuation succeeded).
void TestSidecarPersistFailureIncrementsCounterNotBreaker() {
    const std::filesystem::path home = MakeTempHome("persist_fail_counter");
    svg_mb_control::ControlRuntimeContext context = MakeContext(home);
    svg_mb_control::ChannelState channel =
        MakeReadyChannel(/*breaker_open=*/false);
    ThrowingPendingWritesStore store;
    RecordingFanWriter writer;

    const svg_mb_control::ChannelEvaluation eval =
        MakeEvaluation(60.0, /*safety_override=*/false);
    RunWrite(context, channel, eval, writer, store);

    ExpectTrue(channel.consecutive_sidecar_persist_failures == 1u,
               "a persist failure increments the sidecar-persist-failure counter");
    ExpectTrue(channel.consecutive_write_failures == 0u,
               "a sidecar persist failure does not increment write failures");
    ExpectTrue(!channel.circuit_breaker_open,
               "a sidecar persist failure does not open the write-failure breaker");

    std::error_code ec;
    std::filesystem::remove_all(home, ec);
}

// FEAT-0010 REQ-WRITESAFE-03: the persist-failure counter resets once a persist
// succeeds (the store self-heals).
void TestSidecarPersistFailureCounterResetsOnSuccess() {
    const std::filesystem::path home = MakeTempHome("persist_fail_reset");
    svg_mb_control::ControlRuntimeContext context = MakeContext(home);
    svg_mb_control::ChannelState channel =
        MakeReadyChannel(/*breaker_open=*/false);
    channel.consecutive_sidecar_persist_failures = 3u;  // simulate prior faults
    svg_mb_control::PendingWritesStore working_store(home);
    RecordingFanWriter writer;

    const svg_mb_control::ChannelEvaluation eval =
        MakeEvaluation(60.0, /*safety_override=*/false);
    RunWrite(context, channel, eval, writer, working_store);

    ExpectTrue(channel.consecutive_sidecar_persist_failures == 0u,
               "a successful persist resets the sidecar-persist-failure counter");
    ExpectTrue(writer.apply_calls.size() == 1u,
               "the successful write still actuates");

    std::error_code ec;
    std::filesystem::remove_all(home, ec);
}

// FEAT-0010 REQ-WRITESAFE-03: a sidecar persist failure degrades runtime
// health — a channel carrying a nonzero persist-failure counter (and nothing
// else) counts toward the degraded-channel total that drives the health state.
void TestSidecarPersistFailureDegradesHealth() {
    svg_mb_control::RuntimeStatusSnapshot snapshot;
    svg_mb_control::RuntimeStatusChannelSnapshot ch;
    ch.channel = 2u;
    ch.consecutive_sidecar_persist_failures = 1u;  // only this set
    snapshot.controlled_channels.push_back(ch);

    ExpectTrue(snapshot.DegradedChannelCount() == 1u,
               "a sidecar persist failure degrades the channel health count");
}

// FEAT-0010 REQ-WRITESAFE-04: crash recovery stays correct when the sidecar
// entry is stale or absent because a persist failed. The captured baseline
// (baseline_duty_raw/mode_raw) is stable across ticks, so a stale-but-present
// entry still round-trips the baseline that reconcile/restore replays; an
// absent sidecar yields no entries (the accepted first-write residual) without
// error.
void TestSidecarBaselineSurvivesStaleAndAbsentEntry() {
    const std::filesystem::path home = MakeTempHome("recovery_baseline");

    const std::vector<svg_mb_control::PendingWriteEntry> absent =
        svg_mb_control::ReadPendingWrites(home);
    ExpectTrue(absent.empty(),
               "an absent sidecar yields no pending entries (first-write residual)");

    svg_mb_control::PendingWriteEntry entry;
    entry.channel = 2u;
    entry.baseline_duty_raw = 42u;
    entry.baseline_mode_raw = 1u;
    entry.target_pct = 80.0;
    svg_mb_control::WritePendingWrites(home, {entry});

    const std::vector<svg_mb_control::PendingWriteEntry> reread =
        svg_mb_control::ReadPendingWrites(home);
    ExpectTrue(reread.size() == 1u,
               "a stale sidecar entry stays readable for reconcile");
    if (reread.size() == 1u) {
        ExpectTrue(reread.front().baseline_duty_raw == 42u &&
                       reread.front().baseline_mode_raw == 1u,
                   "the captured baseline survives in a stale sidecar entry");
    }

    std::error_code ec;
    std::filesystem::remove_all(home, ec);
}

// FEAT-0010 REQ-WRITESAFE-03: the persist-failure path emits the
// control_loop.sidecar_upsert_failed runtime event.
void TestSidecarPersistFailureEmitsEvent() {
    const std::filesystem::path home = MakeTempHome("persist_fail_event");
    svg_mb_control::ControlRuntimeContext context = MakeContext(home);
    svg_mb_control::ChannelState channel =
        MakeReadyChannel(/*breaker_open=*/false);
    ThrowingPendingWritesStore store;
    RecordingFanWriter writer;

    const svg_mb_control::ChannelEvaluation eval =
        MakeEvaluation(60.0, /*safety_override=*/false);
    RunWrite(context, channel, eval, writer, store);

    ExpectTrue(EventLogContains(home, "control_loop.sidecar_upsert_failed"),
               "the persist-failure path emits control_loop.sidecar_upsert_failed");

    std::error_code ec;
    std::filesystem::remove_all(home, ec);
}

// FEAT-0010 REQ-WRITESAFE-06: event logging on the persist-failure path is
// best-effort and must NOT veto the fan write. A throw from event serialization
// (here, a non-UTF-8 exception message that makes the event JSON dump throw) is
// swallowed and the computed duty is still applied.
void TestEventLogThrowDoesNotVetoActuation() {
    const std::filesystem::path home = MakeTempHome("persist_fail_logthrow");
    svg_mb_control::ControlRuntimeContext context = MakeContext(home);
    svg_mb_control::ChannelState channel =
        MakeReadyChannel(/*breaker_open=*/false);
    Utf8ThrowingPendingWritesStore store;
    RecordingFanWriter writer;

    const svg_mb_control::ChannelEvaluation eval =
        MakeEvaluation(60.0, /*safety_override=*/false);
    try {
        RunWrite(context, channel, eval, writer, store);
    } catch (...) {
        // Pre-fix, the pre-actuation event-serialization throw escapes here;
        // post-fix it is swallowed and control never reaches this point.
    }

    ExpectTrue(writer.apply_calls.size() == 1u,
               "an event-log throw on the persist-failure path does not veto actuation");

    std::error_code ec;
    std::filesystem::remove_all(home, ec);
}

// --- FEAT-0013: source-aware CPU-dropout safe mode (REQ-SRCSAFE-*) ---

svg_mb_control::ChannelState MakeSourceAwareChannel() {
    svg_mb_control::ChannelState channel;
    channel.config.channel = 2u;
    channel.config.temp_blend = svg_mb_control::TempBlend::MaxCpuGpuSourceAware;
    channel.config.source_aware_cpu_hot_guard_c = 75.0;
    channel.config.curve = {{30.0, 20.0}, {70.0, 100.0}};
    return channel;
}

svg_mb_control::TempInputs MakeTempInputs(bool cpu_avail, double cpu_c,
                                         bool gpu_avail, double gpu_c) {
    svg_mb_control::TempInputs t;
    t.cpu_available = cpu_avail;
    t.cpu_c = cpu_c;
    t.gpu_available = gpu_avail;
    t.gpu_c = gpu_c;
    return t;
}

// REQ-SRCSAFE-01: a CPU dropout (CPU seen, now gone, GPU still present) on a
// source-aware channel counts toward consecutive_sensor_failures instead of being
// reset by the GPU fallback.
void TestSourceAwareCpuDropoutCountsTowardTrip() {
    using namespace svg_mb_control;
    ControlLoopConfig loop;
    ChannelState channel = MakeSourceAwareChannel();
    RuntimeSnapshot empty_snapshot;
    RuntimeSnapshotIndex index;
    index.Rebuild(empty_snapshot);
    const auto now = std::chrono::steady_clock::now();

    EvaluateChannel(channel, loop, MakeTempInputs(true, 55.0, true, 50.0), index,
                    now);  // CPU seen, both present
    ExpectTrue(channel.consecutive_sensor_failures == 0u,
               "no sensor-failure count while both inputs are present");

    EvaluateChannel(channel, loop, MakeTempInputs(false, 0.0, true, 50.0), index,
                    now);  // CPU drops, GPU remains
    ExpectTrue(channel.consecutive_sensor_failures == 1u,
               "a CPU dropout on a source-aware channel increments the sensor-failure count");
}

// REQ-SRCSAFE-02/03: three consecutive CPU-dropout ticks trip safe mode
// (safety_override + a distinct response source) and emit FailureDetected.
void TestSourceAwareCpuDropoutTripsSafeMode() {
    using namespace svg_mb_control;
    ControlLoopConfig loop;
    ChannelState channel = MakeSourceAwareChannel();
    RuntimeSnapshot empty_snapshot;
    RuntimeSnapshotIndex index;
    index.Rebuild(empty_snapshot);
    const auto now = std::chrono::steady_clock::now();

    EvaluateChannel(channel, loop, MakeTempInputs(true, 55.0, true, 50.0), index,
                    now);  // seed CPU-seen
    const TempInputs dropout = MakeTempInputs(false, 0.0, true, 50.0);
    const ChannelEvaluation d1 = EvaluateChannel(channel, loop, dropout, index, now);
    ExpectTrue(!d1.safety_override, "no safe mode before the dropout threshold (1)");
    const ChannelEvaluation d2 = EvaluateChannel(channel, loop, dropout, index, now);
    ExpectTrue(!d2.safety_override, "no safe mode before the dropout threshold (2)");
    const ChannelEvaluation d3 = EvaluateChannel(channel, loop, dropout, index, now);
    ExpectTrue(d3.safety_override,
               "three CPU-dropout ticks trip source-aware safe mode");
    ExpectTrue(d3.response_source == "source_aware_cpu_dropout_safe_mode",
               "the dropout trip carries a distinct response source");
    ExpectTrue(d3.sensor_event == ChannelSensorEvent::FailureDetected,
               "the dropout trip emits FailureDetected");
    ExpectTrue(d3.has_setpoint, "the dropout safe-mode command produces a setpoint");
}

// REQ-SRCSAFE-03: when CPU returns after a dropout trip, the channel recovers
// (Recovered event, safety_override cleared, counter reset).
void TestSourceAwareCpuRecoveryClearsDropout() {
    using namespace svg_mb_control;
    ControlLoopConfig loop;
    ChannelState channel = MakeSourceAwareChannel();
    RuntimeSnapshot empty_snapshot;
    RuntimeSnapshotIndex index;
    index.Rebuild(empty_snapshot);
    const auto now = std::chrono::steady_clock::now();

    EvaluateChannel(channel, loop, MakeTempInputs(true, 55.0, true, 50.0), index,
                    now);  // seed
    const TempInputs dropout = MakeTempInputs(false, 0.0, true, 50.0);
    EvaluateChannel(channel, loop, dropout, index, now);
    EvaluateChannel(channel, loop, dropout, index, now);
    EvaluateChannel(channel, loop, dropout, index, now);  // trip

    const ChannelEvaluation rec = EvaluateChannel(
        channel, loop, MakeTempInputs(true, 55.0, true, 50.0), index, now);
    ExpectTrue(rec.sensor_event == ChannelSensorEvent::Recovered,
               "CPU return after a dropout trip emits Recovered");
    ExpectTrue(!rec.safety_override, "CPU recovery clears safe mode");
    ExpectTrue(channel.consecutive_sensor_failures == 0u,
               "CPU recovery resets the sensor-failure counter");
}

// REQ-SRCSAFE-04: a source-aware channel that never saw CPU (GPU-led from the
// start) does not trip the CPU-dropout safe mode.
void TestSourceAwareNeverPresentCpuDoesNotTrip() {
    using namespace svg_mb_control;
    ControlLoopConfig loop;
    ChannelState channel = MakeSourceAwareChannel();
    RuntimeSnapshot empty_snapshot;
    RuntimeSnapshotIndex index;
    index.Rebuild(empty_snapshot);
    const auto now = std::chrono::steady_clock::now();

    const TempInputs gpu_only = MakeTempInputs(false, 0.0, true, 50.0);
    for (int i = 0; i < 5; ++i) {
        const ChannelEvaluation e =
            EvaluateChannel(channel, loop, gpu_only, index, now);
        ExpectTrue(!e.safety_override,
                   "a never-CPU-present source-aware channel does not trip dropout safe mode");
    }
    ExpectTrue(channel.consecutive_sensor_failures == 0u,
               "never-present CPU does not accumulate sensor failures");
}

// REQ-SRCSAFE-05 (regression): with both inputs present, the source-aware channel
// rides the curve normally and never enters safe mode.
void TestSourceAwareBothPresentNoTrip() {
    using namespace svg_mb_control;
    ControlLoopConfig loop;
    ChannelState channel = MakeSourceAwareChannel();
    RuntimeSnapshot empty_snapshot;
    RuntimeSnapshotIndex index;
    index.Rebuild(empty_snapshot);
    const auto now = std::chrono::steady_clock::now();

    for (int i = 0; i < 5; ++i) {
        const ChannelEvaluation e = EvaluateChannel(
            channel, loop, MakeTempInputs(true, 55.0, true, 50.0), index, now);
        ExpectTrue(!e.safety_override, "both inputs present: no safe mode");
        ExpectTrue(channel.consecutive_sensor_failures == 0u,
                   "both inputs present: no sensor-failure accumulation");
    }
}

}  // namespace

int main() {
    TestSensorSafeBypassesOpenBreaker();
    TestNormalWriteSuppressedByOpenBreaker();
    TestNormalWriteAppliesWhenBreakerClosed();
    TestSafetyWriteFailureKeepsBreakerOpen();
    TestEvaluatorSetsSafetyOverrideOnSensorFailure();
    TestSidecarPersistFailureStillActuates();
    TestSafetyOverrideActuatesDespiteSidecarPersistFailure();
    TestSidecarPersistFailureIncrementsCounterNotBreaker();
    TestSidecarPersistFailureCounterResetsOnSuccess();
    TestSidecarPersistFailureDegradesHealth();
    TestSidecarBaselineSurvivesStaleAndAbsentEntry();
    TestSidecarPersistFailureEmitsEvent();
    TestEventLogThrowDoesNotVetoActuation();
    TestSourceAwareCpuDropoutCountsTowardTrip();
    TestSourceAwareCpuDropoutTripsSafeMode();
    TestSourceAwareCpuRecoveryClearsDropout();
    TestSourceAwareNeverPresentCpuDoesNotTrip();
    TestSourceAwareBothPresentNoTrip();

    if (g_failures != 0) {
        std::cerr << g_failures << " channel write test failure(s)\n";
        return 1;
    }
    return 0;
}
