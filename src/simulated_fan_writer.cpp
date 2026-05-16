// Simulation fan-writer strategy. Split out of fan_writer.cpp so the hermetic
// test hook is a discrete translation unit, selected by CreateFanWriter when
// SVG_MB_CONTROL_SIM_DIRECT_WRITE_MODE=enabled.

#include "fan_writer_internal.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace svg_mb_control {

namespace {

std::string GetEnvOrDefault(const char* name, std::string_view fallback) {
    char* value = nullptr;
    std::size_t size = 0u;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr ||
        value[0] == '\0') {
        if (value != nullptr) {
            std::free(value);
        }
        return std::string(fallback);
    }
    std::string result(value);
    std::free(value);
    return result;
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
        const std::uint32_t sim_channel = static_cast<std::uint32_t>(
            std::stoul(GetEnvOrDefault("SVG_MB_CONTROL_SIM_READ_FAN_CHANNEL",
                                       GetEnvOrDefault("SVG_MB_CONTROL_SIM_FAN_CHANNEL",
                                                       "0"))));
        if (channel != sim_channel) {
            return MakeReadOk(state);
        }
        state.present = true;
        state.duty_raw = static_cast<std::uint8_t>(
            std::stoul(GetEnvOrDefault("SVG_MB_CONTROL_SIM_READ_FAN_DUTY_RAW",
                                       GetEnvOrDefault("SVG_MB_CONTROL_SIM_FAN_DUTY_RAW",
                                                       "128"))));
        state.mode_raw = static_cast<std::uint8_t>(
            std::stoul(GetEnvOrDefault("SVG_MB_CONTROL_SIM_READ_FAN_MODE_RAW",
                                       GetEnvOrDefault("SVG_MB_CONTROL_SIM_FAN_MODE_RAW",
                                                       "5"))));
        state.tach_hi_raw = static_cast<std::uint8_t>(
            std::stoul(GetEnvOrDefault("SVG_MB_CONTROL_SIM_READ_FAN_TACH_HI_RAW",
                                       GetEnvOrDefault("SVG_MB_CONTROL_SIM_FAN_TACH_HI_RAW",
                                                       "4"))));
        state.tach_lo_raw = static_cast<std::uint8_t>(
            std::stoul(GetEnvOrDefault("SVG_MB_CONTROL_SIM_READ_FAN_TACH_LO_RAW",
                                       GetEnvOrDefault("SVG_MB_CONTROL_SIM_FAN_TACH_LO_RAW",
                                                       "18"))));
        state.tach_raw = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(state.tach_hi_raw) << 5u) |
            (static_cast<std::uint16_t>(state.tach_lo_raw) & 0x1Fu));
        state.tach_valid = GetEnvOrDefault(
            "SVG_MB_CONTROL_SIM_READ_FAN_TACH_VALID", "true") != "false";
        state.rpm = static_cast<std::uint16_t>(
            std::stoul(GetEnvOrDefault("SVG_MB_CONTROL_SIM_READ_FAN_RPM",
                                       "1200")));
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

        const std::uint32_t channel = static_cast<std::uint32_t>(
            std::stoul(GetEnvOrDefault("SVG_MB_CONTROL_SIM_READ_FAN_CHANNEL",
                                       GetEnvOrDefault("SVG_MB_CONTROL_SIM_FAN_CHANNEL",
                                                       "0"))));
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

        const std::uint32_t channel = static_cast<std::uint32_t>(
            std::stoul(GetEnvOrDefault("SVG_MB_CONTROL_SIM_READ_FAN_CHANNEL",
                                       GetEnvOrDefault("SVG_MB_CONTROL_SIM_FAN_CHANNEL",
                                                       "0"))));
        FanTachEvidenceState state;
        state.channel = channel;
        state.tach_hi_raw = static_cast<std::uint8_t>(
            std::stoul(GetEnvOrDefault("SVG_MB_CONTROL_SIM_READ_FAN_TACH_HI_RAW",
                                       GetEnvOrDefault("SVG_MB_CONTROL_SIM_FAN_TACH_HI_RAW",
                                                       "4"))));
        state.tach_lo_raw = static_cast<std::uint8_t>(
            std::stoul(GetEnvOrDefault("SVG_MB_CONTROL_SIM_READ_FAN_TACH_LO_RAW",
                                       GetEnvOrDefault("SVG_MB_CONTROL_SIM_FAN_TACH_LO_RAW",
                                                       "18"))));
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

        const std::uint32_t count = static_cast<std::uint32_t>(
            std::stoul(GetEnvOrDefault(
                "SVG_MB_CONTROL_SIM_SIO_VOLTAGE_COUNT", "2")));
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
            state.raw = static_cast<std::uint8_t>(
                std::stoul(GetEnvOrDefault(
                    (prefix + "_RAW").c_str(),
                    index == 0u ? "150" : "206")));
            state.voltage_v = std::stod(GetEnvOrDefault(
                (prefix + "_V").c_str(),
                index == 0u ? "1.200" : "3.296"));
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

        const std::uint32_t count = static_cast<std::uint32_t>(
            std::stoul(GetEnvOrDefault(
                "SVG_MB_CONTROL_SIM_SIO_TEMPERATURE_COUNT", "1")));
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
                std::stoul(GetEnvOrDefault((prefix + "_RAW").c_str(), "61")));
            state.half_raw = static_cast<std::uint8_t>(
                std::stoul(GetEnvOrDefault((prefix + "_HALF_RAW").c_str(), "0")));
            state.temperature_c = std::stod(GetEnvOrDefault(
                (prefix + "_C").c_str(), "61.0"));
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
        const std::uint32_t delay_ms = static_cast<std::uint32_t>(
            std::stoul(GetEnvOrDefault("SVG_MB_CONTROL_SIM_RESTORE_DELAY_MS",
                                       "0")));
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
