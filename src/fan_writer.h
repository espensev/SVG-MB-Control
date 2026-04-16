#pragma once

#include "runtime_write_policy.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace svg_mb_control {

enum class FanWriteError {
    kNone = 0,
    kPolicyRefused,
    kUnavailable,
    kInvalidChannel,
    kWriteFailed,
    kRestoreFailed,
};

struct FanWriteResult {
    FanWriteError error = FanWriteError::kNone;
    std::string detail;

    bool ok() const { return error == FanWriteError::kNone; }
    explicit operator bool() const { return ok(); }
};

struct FanChannelState {
    bool present = false;
    std::uint32_t channel = 0u;
    std::uint16_t rpm = 0u;
    std::uint16_t tach_raw = 0u;
    std::uint8_t duty_raw = 0u;
    std::uint8_t mode_raw = 0u;
    double duty_percent = 0.0;
    bool tach_valid = false;
    bool manual_override = false;
    std::string label;
};

struct FanReadResult {
    FanWriteError error = FanWriteError::kNone;
    std::string detail;
    FanChannelState state;

    bool ok() const { return error == FanWriteError::kNone; }
    explicit operator bool() const { return ok(); }
};

struct FanScanResult {
    FanWriteError error = FanWriteError::kNone;
    std::string detail;
    std::vector<FanChannelState> fans;

    bool ok() const { return error == FanWriteError::kNone; }
    explicit operator bool() const { return ok(); }
};

class FanWriter {
  public:
    virtual ~FanWriter() = default;

    virtual FanReadResult ReadChannelState(std::uint32_t channel) = 0;
    virtual FanScanResult ReadAllChannels() = 0;
    virtual FanWriteResult ApplyDuty(std::uint32_t channel,
                                     double duty_pct) = 0;
    virtual FanWriteResult RestoreSavedState(std::uint32_t channel,
                                             std::uint8_t duty_raw,
                                             std::uint8_t mode_raw) = 0;
    virtual std::string BackendLabel() const = 0;
};

std::unique_ptr<FanWriter> CreateFanWriter(
    const RuntimeWritePolicy& runtime_policy);

}  // namespace svg_mb_control
