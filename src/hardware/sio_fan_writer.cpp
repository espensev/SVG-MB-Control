// Super I/O fan-writer strategy backed by the vendored svg_mb_sio controller.
// Split out of fan_writer.cpp; the default backend selected by
// CreateFanWriter. StatusString/TranslateStatus are Sio-specific and stay
// local to this translation unit.

#include "fan_writer_internal.h"

#include "svg_mb_sio/svg_mb_sio.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace svg_mb_control {

namespace {

constexpr std::array<std::uint16_t, 7u> kNct6701FanCountRegisters = {
    0x4B0u, 0x4B2u, 0x4B4u, 0x4B6u, 0x4B8u, 0x4BAu, 0x4CCu
};
constexpr unsigned int kSioTransientAttempts = 3u;
constexpr unsigned int kSioTransientRetryDelayMs = 75u;
constexpr unsigned int kSioInitAttempts = 5u;
constexpr unsigned int kSioInitRetryDelayMs = 250u;

std::string StatusString(MbSioStatus status) {
    return std::string(mb_sio_status_string(status));
}

bool IsTransientSioStatus(MbSioStatus status) {
    return status == MbSioStatus::access_denied ||
           status == MbSioStatus::error ||
           status == MbSioStatus::timeout;
}

template <typename Operation>
MbSioStatus RetryTransientSioOperation(Operation&& operation) {
    MbSioStatus status = MbSioStatus::error;
    for (unsigned int attempt = 0u; attempt < kSioTransientAttempts;
         ++attempt) {
        status = operation();
        if (status == MbSioStatus::ok || !IsTransientSioStatus(status) ||
            attempt + 1u >= kSioTransientAttempts) {
            return status;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kSioTransientRetryDelayMs));
    }
    return status;
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

class SioFanWriter final : public FanWriter {
  public:
    explicit SioFanWriter(const RuntimeWritePolicy& runtime_policy) {
        MbSioWritePolicy policy;
        policy.writes_enabled = runtime_policy.writes_enabled;
        policy.restore_on_exit = false;
        policy.blocked_channels = runtime_policy.blocked_channels;

        // Chip detection can fail transiently when another hardware monitor
        // probes the Super I/O config space concurrently (observed
        // 2026-06-11: the boot-time worker lost the NCT6701D probe while a
        // sibling fan tool was polling, and the supervisor treats a startup
        // failure as fatal). init() begins with an internal close(), so
        // re-running the init+discover sequence is safe.
        std::string warning;
        for (unsigned int attempt = 1u;; ++attempt) {
            warning.clear();
            const bool init_ok = controller_.init(policy, warning);
            if (init_ok) {
                device_ = controller_.discover();
                if (device_.sio_available) {
                    return;
                }
            }
            if (attempt >= kSioInitAttempts) {
                if (!init_ok) {
                    if (warning.empty()) {
                        warning = "unknown init failure";
                    }
                    throw std::runtime_error("svg_mb_sio init failed: " +
                                             warning);
                }
                throw std::runtime_error(
                    "svg_mb_sio did not discover a supported Super I/O "
                    "device.");
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kSioInitRetryDelayMs));
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
        const MbSioStatus status = RetryTransientSioOperation([&]() {
            return controller_.read_fans(device_, fans);
        });
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

    void ReadAllChannels(FanScanResult& out) override {
        out.error = FanWriteError::kNone;
        out.detail.clear();
        out.fans.clear();

        std::vector<MbFanSnapshot> fans;
        const MbSioStatus status = RetryTransientSioOperation([&]() {
            return controller_.read_fans(device_, fans);
        });
        if (status != MbSioStatus::ok) {
            out.error = TranslateStatus(status, 0u, "read_fans").error;
            out.detail = "svg_mb_sio read_fans failed: " + StatusString(status);
            return;
        }

        out.fans.reserve(fans.size());
        for (const auto& fan : fans) {
            out.fans.push_back(TranslateFan(fan));
        }
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
            MbSioStatus status = RetryTransientSioOperation([&]() {
                return controller_.read_raw_register(device_, hi_reg, &hi_raw);
            });
            if (status == MbSioStatus::ok) {
                status = RetryTransientSioOperation([&]() {
                    return controller_.read_raw_register(
                        device_, static_cast<std::uint16_t>(hi_reg + 1u),
                        &lo_raw);
                });
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
        const MbSioStatus status = RetryTransientSioOperation([&]() {
            return controller_.read_voltages(device_, voltages);
        });
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
        const MbSioStatus status = RetryTransientSioOperation([&]() {
            return controller_.read_sio_temperatures(device_, temperatures);
        });
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
        const MbSioStatus status = RetryTransientSioOperation([&]() {
            return controller_.set_fan_duty(device_, channel, duty_pct);
        });
        return TranslateStatus(
            status, channel, "set_fan_duty");
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
        const MbSioStatus status = RetryTransientSioOperation([&]() {
            return controller_.restore_saved_state(
                device_, channel, duty_raw, mode_raw, timeout_ms);
        });
        return TranslateStatus(
            status, channel, "restore_saved_state");
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

std::unique_ptr<FanWriter> MakeSioFanWriter(
    const RuntimeWritePolicy& runtime_policy) {
    return std::make_unique<SioFanWriter>(runtime_policy);
}

}  // namespace svg_mb_control
