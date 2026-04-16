#include "fan_writer.h"

#include "svg_mb_sio/svg_mb_sio.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

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
                                     std::uint8_t mode_raw) override {
        (void)channel;
        (void)duty_raw;
        (void)mode_raw;
        const std::string mode = GetEnvOrDefault(
            "SVG_MB_CONTROL_SIM_RESTORE_MODE", "success");
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
                                     std::uint8_t mode_raw) override {
        if (channel >= device_.sio.fan_count) {
            return MakeResult(
                FanWriteError::kInvalidChannel,
                "svg_mb_sio restore_saved_state failed for channel " +
                    std::to_string(channel) + ": channel out of range");
        }
        return TranslateStatus(
            controller_.restore_saved_state(device_, channel, duty_raw, mode_raw),
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
