// Simulation fan-writer strategy. Split out of fan_writer.cpp so the hermetic
// test hook is a discrete translation unit, selected by CreateFanWriter when
// SVG_MB_CONTROL_SIM_DIRECT_WRITE_MODE=enabled.

#include "env_util.h"
#include "fan_writer_internal.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace svg_mb_control {

namespace {

// Robust env-var parsing helpers: malformed values fall back to the supplied
// default instead of throwing std::invalid_argument / std::out_of_range
// across the simulated read path. The previous std::stoul/std::stod chains
// would terminate the simulator on a typo in a test harness env var.
std::uint32_t ParseEnvUInt32(const char* name, std::uint32_t fallback) {
    const std::string value =
        GetEnvOrDefault(name, std::string_view{});
    if (value.empty()) {
        return fallback;
    }
    try {
        const unsigned long parsed = std::stoul(value);
        if (parsed > static_cast<unsigned long>(UINT32_MAX)) {
            return fallback;
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::uint32_t ParseEnvUInt32(const char* primary_name,
                             const char* fallback_name,
                             std::uint32_t fallback) {
    const std::string value =
        GetEnvOrDefault(primary_name, std::string_view{});
    if (!value.empty()) {
        return ParseEnvUInt32(primary_name, fallback);
    }
    return ParseEnvUInt32(fallback_name, fallback);
}

double ParseEnvDouble(const char* name, double fallback) {
    const std::string value =
        GetEnvOrDefault(name, std::string_view{});
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

class SimulatedFanWriter final : public FanWriter {
  public:
    explicit SimulatedFanWriter(RuntimeWritePolicy policy)
        : policy_(std::move(policy)) {}

    FanReadResult ReadChannelState(std::uint32_t channel) override {
        const std::string mode = GetEnvOrDefault(
            "SVG_MB_CONTROL_SIM_READ_MODE", "success");
        if (mode == "fail") {
            return MakeReadResult(FanWriteError::kUnavailable,
                                  "simulated direct fan read failure");
        }

        FanChannelState state;
        state.channel = channel;
        const std::uint32_t sim_channel = ParseEnvUInt32(
            "SVG_MB_CONTROL_SIM_READ_FAN_CHANNEL",
            "SVG_MB_CONTROL_SIM_FAN_CHANNEL", 0u);
        if (channel != sim_channel) {
            return MakeReadOk(state);
        }
        state.present = true;
        state.duty_raw = static_cast<std::uint8_t>(ParseEnvUInt32(
            "SVG_MB_CONTROL_SIM_READ_FAN_DUTY_RAW",
            "SVG_MB_CONTROL_SIM_FAN_DUTY_RAW", 128u));
        state.mode_raw = static_cast<std::uint8_t>(ParseEnvUInt32(
            "SVG_MB_CONTROL_SIM_READ_FAN_MODE_RAW",
            "SVG_MB_CONTROL_SIM_FAN_MODE_RAW", 5u));
        state.tach_hi_raw = static_cast<std::uint8_t>(ParseEnvUInt32(
            "SVG_MB_CONTROL_SIM_READ_FAN_TACH_HI_RAW",
            "SVG_MB_CONTROL_SIM_FAN_TACH_HI_RAW", 4u));
        state.tach_lo_raw = static_cast<std::uint8_t>(ParseEnvUInt32(
            "SVG_MB_CONTROL_SIM_READ_FAN_TACH_LO_RAW",
            "SVG_MB_CONTROL_SIM_FAN_TACH_LO_RAW", 18u));
        state.tach_raw = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(state.tach_hi_raw) << 5u) |
            (static_cast<std::uint16_t>(state.tach_lo_raw) & 0x1Fu));
        state.tach_valid = GetEnvOrDefault(
            "SVG_MB_CONTROL_SIM_READ_FAN_TACH_VALID", "true") != "false";
        state.rpm = static_cast<std::uint16_t>(ParseEnvUInt32(
            "SVG_MB_CONTROL_SIM_READ_FAN_RPM", 1200u));
        state.duty_percent =
            static_cast<double>(state.duty_raw) * (100.0 / 255.0);
        state.label = "sim-channel-" + std::to_string(channel);
        return MakeReadOk(state);
    }

    FanScanResult ReadAllChannels() override {
        const std::string mode = GetEnvOrDefault(
            "SVG_MB_CONTROL_SIM_READ_MODE", "success");
        if (mode == "fail") {
            return MakeScanResult(FanWriteError::kUnavailable,
                                  "simulated direct fan read failure");
        }

        const std::uint32_t channel = ParseEnvUInt32(
            "SVG_MB_CONTROL_SIM_READ_FAN_CHANNEL",
            "SVG_MB_CONTROL_SIM_FAN_CHANNEL", 0u);
        const FanReadResult read_result = ReadChannelState(channel);
        if (!read_result) {
            return MakeScanResult(read_result.error, read_result.detail);
        }

        std::vector<FanChannelState> fans;
        if (read_result.state.present) {
            fans.push_back(read_result.state);
        }
        return MakeScanOk(std::move(fans));
    }

    FanTachEvidenceScanResult ReadFanTachEvidence() override {
        const std::string mode = GetEnvOrDefault(
            "SVG_MB_CONTROL_SIM_SIO_EVIDENCE_MODE", "success");
        if (mode == "fail" || mode == "tach_fail") {
            return MakeFanTachEvidenceResult(
                FanWriteError::kUnavailable,
                "simulated fan tach evidence failure");
        }

        const std::uint32_t channel = ParseEnvUInt32(
            "SVG_MB_CONTROL_SIM_READ_FAN_CHANNEL",
            "SVG_MB_CONTROL_SIM_FAN_CHANNEL", 0u);
        FanTachEvidenceState state;
        state.channel = channel;
        state.tach_hi_raw = static_cast<std::uint8_t>(ParseEnvUInt32(
            "SVG_MB_CONTROL_SIM_READ_FAN_TACH_HI_RAW",
            "SVG_MB_CONTROL_SIM_FAN_TACH_HI_RAW", 4u));
        state.tach_lo_raw = static_cast<std::uint8_t>(ParseEnvUInt32(
            "SVG_MB_CONTROL_SIM_READ_FAN_TACH_LO_RAW",
            "SVG_MB_CONTROL_SIM_FAN_TACH_LO_RAW", 18u));
        return MakeFanTachEvidenceOk({state});
    }

    SioVoltageScanResult ReadVoltages() override {
        const std::string mode = GetEnvOrDefault(
            "SVG_MB_CONTROL_SIM_SIO_EVIDENCE_MODE", "success");
        if (mode == "fail" || mode == "voltage_fail") {
            return MakeVoltageScanResult(
                FanWriteError::kUnavailable,
                "simulated SIO voltage evidence failure");
        }

        const std::uint32_t count = ParseEnvUInt32(
            "SVG_MB_CONTROL_SIM_SIO_VOLTAGE_COUNT", 2u);
        std::vector<SioVoltageState> voltages;
        voltages.reserve(count);
        for (std::uint32_t index = 0u; index < count; ++index) {
            SioVoltageState state;
            state.index = index;
            const std::string prefix =
                "SVG_MB_CONTROL_SIM_SIO_VOLTAGE" + std::to_string(index);
            state.label = GetEnvOrDefault(
                (prefix + "_LABEL").c_str(),
                index == 0u ? "sim-vcore" : "sim-3v3");
            state.raw = static_cast<std::uint8_t>(ParseEnvUInt32(
                (prefix + "_RAW").c_str(),
                index == 0u ? 150u : 206u));
            state.voltage_v = ParseEnvDouble(
                (prefix + "_V").c_str(),
                index == 0u ? 1.200 : 3.296);
            voltages.push_back(std::move(state));
        }
        return MakeVoltageScanOk(std::move(voltages));
    }

    SioTemperatureScanResult ReadSioTemperatures() override {
        const std::string mode = GetEnvOrDefault(
            "SVG_MB_CONTROL_SIM_SIO_EVIDENCE_MODE", "success");
        if (mode == "fail" || mode == "temperature_fail") {
            return MakeTemperatureScanResult(
                FanWriteError::kUnavailable,
                "simulated SIO temperature evidence failure");
        }

        const std::uint32_t count = ParseEnvUInt32(
            "SVG_MB_CONTROL_SIM_SIO_TEMPERATURE_COUNT", 1u);
        std::vector<SioTemperatureState> temperatures;
        temperatures.reserve(count);
        for (std::uint32_t index = 0u; index < count; ++index) {
            SioTemperatureState state;
            state.index = index;
            const std::string prefix =
                "SVG_MB_CONTROL_SIM_SIO_TEMPERATURE" + std::to_string(index);
            state.label = GetEnvOrDefault(
                (prefix + "_LABEL").c_str(),
                index == 0u ? "sim-sio-temp0" : "sim-sio-temp");
            state.raw = static_cast<std::uint8_t>(
                ParseEnvUInt32((prefix + "_RAW").c_str(), 61u));
            state.half_raw = static_cast<std::uint8_t>(
                ParseEnvUInt32((prefix + "_HALF_RAW").c_str(), 0u));
            state.temperature_c = ParseEnvDouble(
                (prefix + "_C").c_str(), 61.0);
            state.valid = GetEnvOrDefault(
                (prefix + "_VALID").c_str(), "true") != "false";
            temperatures.push_back(std::move(state));
        }
        return MakeTemperatureScanOk(std::move(temperatures));
    }

    FanWriteResult ApplyDuty(std::uint32_t channel,
                             double duty_pct) override {
        (void)duty_pct;
        if (!policy_.writes_enabled ||
            RuntimeWritePolicyBlocksChannel(policy_, channel)) {
            return MakeResult(FanWriteError::kPolicyRefused,
                              "simulated direct write policy refusal");
        }
        const std::string mode = GetEnvOrDefault(
            "SVG_MB_CONTROL_SIM_WRITE_MODE", "success");
        if (mode == "fail_immediate") {
            return MakeResult(FanWriteError::kWriteFailed,
                              "simulated direct write failure");
        }
        if (mode == "policy_refused") {
            return MakeResult(FanWriteError::kPolicyRefused,
                              "simulated direct write policy refusal");
        }
        return MakeOk();
    }

    FanWriteResult RestoreSavedState(std::uint32_t channel,
                                     std::uint8_t duty_raw,
                                     std::uint8_t mode_raw,
                                     std::uint32_t timeout_ms) override {
        (void)channel;
        (void)duty_raw;
        (void)mode_raw;
        const std::string mode = GetEnvOrDefault(
            "SVG_MB_CONTROL_SIM_RESTORE_MODE", "success");
        const std::uint32_t delay_ms = ParseEnvUInt32(
            "SVG_MB_CONTROL_SIM_RESTORE_DELAY_MS", 0u);
        if (delay_ms > 0u) {
            const std::uint32_t bounded_delay_ms =
                delay_ms > timeout_ms ? timeout_ms : delay_ms;
            if (bounded_delay_ms > 0u) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(bounded_delay_ms));
            }
        }
        if (delay_ms > timeout_ms) {
            return MakeResult(FanWriteError::kTimedOut,
                              "simulated direct restore timed out");
        }
        if (mode == "fail") {
            return MakeResult(FanWriteError::kRestoreFailed,
                              "simulated direct restore failure");
        }
        return MakeOk();
    }

    std::string BackendLabel() const override {
        return "simulated-direct-writer";
    }

  private:
    RuntimeWritePolicy policy_;
};

}  // namespace

std::unique_ptr<FanWriter> MakeSimulatedFanWriter(
    const RuntimeWritePolicy& runtime_policy) {
    return std::make_unique<SimulatedFanWriter>(runtime_policy);
}

}  // namespace svg_mb_control
