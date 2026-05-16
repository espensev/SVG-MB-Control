#include "fan_writer.h"

#include "svg_mb_sio/svg_mb_sio.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace svg_mb_control {

namespace {

constexpr std::array<std::uint16_t, 7u> kNct6701FanCountRegisters = {
    0x4B0u, 0x4B2u, 0x4B4u, 0x4B6u, 0x4B8u, 0x4BAu, 0x4CCu
};

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

FanWriteResult MakeOk() {
    return {};
}

FanWriteResult MakeResult(FanWriteError error, std::string detail) {
    FanWriteResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

FanReadResult MakeReadOk(FanChannelState state) {
    FanReadResult result;
    result.state = state;
    return result;
}

FanReadResult MakeReadResult(FanWriteError error, std::string detail) {
    FanReadResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

FanScanResult MakeScanOk(std::vector<FanChannelState> fans) {
    FanScanResult result;
    result.fans = std::move(fans);
    return result;
}

FanScanResult MakeScanResult(FanWriteError error, std::string detail) {
    FanScanResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

FanTachEvidenceScanResult MakeFanTachEvidenceOk(
    std::vector<FanTachEvidenceState> fans) {
    FanTachEvidenceScanResult result;
    result.fans = std::move(fans);
    return result;
}

FanTachEvidenceScanResult MakeFanTachEvidenceResult(FanWriteError error,
                                                    std::string detail) {
    FanTachEvidenceScanResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

SioVoltageScanResult MakeVoltageScanOk(std::vector<SioVoltageState> voltages) {
    SioVoltageScanResult result;
    result.voltages = std::move(voltages);
    return result;
}

SioVoltageScanResult MakeVoltageScanResult(FanWriteError error,
                                           std::string detail) {
    SioVoltageScanResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

SioTemperatureScanResult MakeTemperatureScanOk(
    std::vector<SioTemperatureState> temperatures) {
    SioTemperatureScanResult result;
    result.temperatures = std::move(temperatures);
    return result;
}

SioTemperatureScanResult MakeTemperatureScanResult(FanWriteError error,
                                                   std::string detail) {
    SioTemperatureScanResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

std::string StatusString(MbSioStatus status) {
    return std::string(mb_sio_status_string(status));
}

FanWriteResult TranslateStatus(MbSioStatus status,
                               std::uint32_t channel,
                               std::string_view operation) {
    if (status == MbSioStatus::ok) {
        return MakeOk();
    }

    std::string detail = "svg_mb_sio ";
    detail += operation;
    detail += " failed for channel ";
    detail += std::to_string(channel);
    detail += ": ";
    detail += StatusString(status);

    switch (status) {
        case MbSioStatus::invalid_arg:
            return MakeResult(FanWriteError::kInvalidChannel, detail);
        case MbSioStatus::not_supported:
            return MakeResult(FanWriteError::kPolicyRefused, detail);
        case MbSioStatus::no_device:
        case MbSioStatus::access_denied:
            return MakeResult(FanWriteError::kUnavailable, detail);
        case MbSioStatus::timeout:
            return MakeResult(FanWriteError::kTimedOut, detail);
        default:
            if (operation == "restore_saved_state") {
                return MakeResult(FanWriteError::kRestoreFailed, detail);
            }
            return MakeResult(FanWriteError::kWriteFailed, detail);
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

class SioFanWriter final : public FanWriter {
  public:
    explicit SioFanWriter(const RuntimeWritePolicy& runtime_policy) {
        MbSioWritePolicy policy;
        policy.writes_enabled = runtime_policy.writes_enabled;
        policy.restore_on_exit = false;
        policy.blocked_channels = runtime_policy.blocked_channels;

        std::string warning;
        if (!controller_.init(policy, warning)) {
            if (warning.empty()) {
                warning = "unknown init failure";
            }
            throw std::runtime_error("svg_mb_sio init failed: " + warning);
        }

        device_ = controller_.discover();
        if (!device_.sio_available) {
            throw std::runtime_error(
                "svg_mb_sio did not discover a supported Super I/O device.");
        }
    }

    FanReadResult ReadChannelState(std::uint32_t channel) override {
        if (channel >= device_.sio.fan_count) {
            return MakeReadResult(
                FanWriteError::kInvalidChannel,
                "svg_mb_sio read_fans failed for channel " +
                    std::to_string(channel) + ": channel out of range");
        }

        std::vector<MbFanSnapshot> fans;
        const MbSioStatus status = controller_.read_fans(device_, fans);
        if (status != MbSioStatus::ok) {
            return MakeReadResult(
                TranslateStatus(status, channel, "read_fans").error,
                "svg_mb_sio read_fans failed for channel " +
                    std::to_string(channel) + ": " + StatusString(status));
        }

        FanChannelState state;
        for (const auto& fan : fans) {
            if (fan.channel == channel) {
                state = TranslateFan(fan);
                break;
            }
        }
        return MakeReadOk(state);
    }

    FanScanResult ReadAllChannels() override {
        std::vector<MbFanSnapshot> fans;
        const MbSioStatus status = controller_.read_fans(device_, fans);
        if (status != MbSioStatus::ok) {
            return MakeScanResult(
                TranslateStatus(status, 0u, "read_fans").error,
                "svg_mb_sio read_fans failed: " + StatusString(status));
        }

        std::vector<FanChannelState> out;
        out.reserve(fans.size());
        for (const auto& fan : fans) {
            out.push_back(TranslateFan(fan));
        }
        return MakeScanOk(std::move(out));
    }

    FanTachEvidenceScanResult ReadFanTachEvidence() override {
        std::vector<FanTachEvidenceState> out;
        out.reserve(device_.sio.fan_count);
        for (std::uint32_t channel = 0u;
             channel < device_.sio.fan_count &&
             channel < kNct6701FanCountRegisters.size();
             ++channel) {
            std::uint8_t hi_raw = 0u;
            std::uint8_t lo_raw = 0u;
            const std::uint16_t hi_reg = kNct6701FanCountRegisters[channel];
            MbSioStatus status = controller_.read_raw_register(
                device_, hi_reg, &hi_raw);
            if (status == MbSioStatus::ok) {
                status = controller_.read_raw_register(
                    device_, static_cast<std::uint16_t>(hi_reg + 1u),
                    &lo_raw);
            }
            if (status != MbSioStatus::ok) {
                return MakeFanTachEvidenceResult(
                    TranslateStatus(status, channel, "read_raw_register").error,
                    "svg_mb_sio fan tach evidence failed for channel " +
                        std::to_string(channel) + ": " +
                        StatusString(status));
            }
            out.push_back(FanTachEvidenceState{
                .channel = channel,
                .tach_hi_raw = hi_raw,
                .tach_lo_raw = lo_raw,
            });
        }
        return MakeFanTachEvidenceOk(std::move(out));
    }

    SioVoltageScanResult ReadVoltages() override {
        std::vector<MbVoltageSnapshot> voltages;
        const MbSioStatus status = controller_.read_voltages(device_, voltages);
        if (status != MbSioStatus::ok) {
            return MakeVoltageScanResult(
                TranslateStatus(status, 0u, "read_voltages").error,
                "svg_mb_sio read_voltages failed: " + StatusString(status));
        }

        std::vector<SioVoltageState> out;
        out.reserve(voltages.size());
        for (const auto& voltage : voltages) {
            SioVoltageState state;
            state.index = voltage.index;
            state.voltage_v = voltage.voltage_v;
            state.raw = voltage.raw;
            state.label = voltage.label;
            out.push_back(std::move(state));
        }
        return MakeVoltageScanOk(std::move(out));
    }

    SioTemperatureScanResult ReadSioTemperatures() override {
        std::vector<MbSioTemperatureSnapshot> temperatures;
        const MbSioStatus status =
            controller_.read_sio_temperatures(device_, temperatures);
        if (status != MbSioStatus::ok) {
            return MakeTemperatureScanResult(
                TranslateStatus(status, 0u, "read_sio_temperatures").error,
                "svg_mb_sio read_sio_temperatures failed: " +
                    StatusString(status));
        }

        std::vector<SioTemperatureState> out;
        out.reserve(temperatures.size());
        for (const auto& temperature : temperatures) {
            SioTemperatureState state;
            state.index = temperature.index;
            state.temperature_c = temperature.temperature_c;
            state.raw = temperature.raw;
            state.half_raw = temperature.half_raw;
            state.valid = temperature.valid;
            state.label = temperature.label;
            out.push_back(std::move(state));
        }
        return MakeTemperatureScanOk(std::move(out));
    }

    FanWriteResult ApplyDuty(std::uint32_t channel,
                             double duty_pct) override {
        if (channel >= device_.sio.fan_count) {
            return MakeResult(
                FanWriteError::kInvalidChannel,
                "svg_mb_sio set_fan_duty failed for channel " +
                    std::to_string(channel) + ": channel out of range");
        }
        return TranslateStatus(
            controller_.set_fan_duty(device_, channel, duty_pct),
            channel, "set_fan_duty");
    }

    FanWriteResult RestoreSavedState(std::uint32_t channel,
                                     std::uint8_t duty_raw,
                                     std::uint8_t mode_raw,
                                     std::uint32_t timeout_ms) override {
        if (channel >= device_.sio.fan_count) {
            return MakeResult(
                FanWriteError::kInvalidChannel,
                "svg_mb_sio restore_saved_state failed for channel " +
                    std::to_string(channel) + ": channel out of range");
        }
        return TranslateStatus(
            controller_.restore_saved_state(
                device_, channel, duty_raw, mode_raw, timeout_ms),
            channel, "restore_saved_state");
    }

    std::string BackendLabel() const override {
        return "svg_mb_sio";
    }

  private:
    static FanChannelState TranslateFan(const MbFanSnapshot& fan) {
        FanChannelState state;
        state.present = true;
        state.channel = fan.channel;
        state.rpm = fan.rpm;
        state.tach_raw = fan.tach_raw;
        state.duty_raw = fan.duty_raw;
        state.mode_raw = fan.mode_raw;
        state.duty_percent = fan.duty_percent;
        state.tach_valid = fan.tach_valid;
        state.manual_override = fan.manual_override;
        state.label = fan.label;
        return state;
    }

    MbSioController controller_;
    MbDeviceDescriptor device_{};
};

}  // namespace

std::unique_ptr<FanWriter> CreateFanWriter(
    const RuntimeWritePolicy& runtime_policy) {
    const std::string sim_mode = GetEnvOrDefault(
        "SVG_MB_CONTROL_SIM_DIRECT_WRITE_MODE", "");
    if (sim_mode == "enabled") {
        return std::make_unique<SimulatedFanWriter>(runtime_policy);
    }

    return std::make_unique<SioFanWriter>(runtime_policy);
}

}  // namespace svg_mb_control
