#pragma once

// Internal shared surface for the fan-writer strategy split. Not part of the
// public API (callers use fan_writer.h / CreateFanWriter). The result-factory
// helpers are header-inline because they are trivial struct constructors used
// by both strategy translation units; the two creators are defined in their
// respective strategy .cpp files and selected by CreateFanWriter.

#include "fan_writer.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace svg_mb_control {

inline FanWriteResult MakeOk() {
    return {};
}

inline FanWriteResult MakeResult(FanWriteError error, std::string detail) {
    FanWriteResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

inline FanReadResult MakeReadOk(FanChannelState state) {
    FanReadResult result;
    result.state = state;
    return result;
}

inline FanReadResult MakeReadResult(FanWriteError error, std::string detail) {
    FanReadResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

inline FanScanResult MakeScanOk(std::vector<FanChannelState> fans) {
    FanScanResult result;
    result.fans = std::move(fans);
    return result;
}

inline FanScanResult MakeScanResult(FanWriteError error, std::string detail) {
    FanScanResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

inline FanTachEvidenceScanResult MakeFanTachEvidenceOk(
    std::vector<FanTachEvidenceState> fans) {
    FanTachEvidenceScanResult result;
    result.fans = std::move(fans);
    return result;
}

inline FanTachEvidenceScanResult MakeFanTachEvidenceResult(FanWriteError error,
                                                           std::string detail) {
    FanTachEvidenceScanResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

inline SioVoltageScanResult MakeVoltageScanOk(
    std::vector<SioVoltageState> voltages) {
    SioVoltageScanResult result;
    result.voltages = std::move(voltages);
    return result;
}

inline SioVoltageScanResult MakeVoltageScanResult(FanWriteError error,
                                                  std::string detail) {
    SioVoltageScanResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

inline SioTemperatureScanResult MakeTemperatureScanOk(
    std::vector<SioTemperatureState> temperatures) {
    SioTemperatureScanResult result;
    result.temperatures = std::move(temperatures);
    return result;
}

inline SioTemperatureScanResult MakeTemperatureScanResult(FanWriteError error,
                                                          std::string detail) {
    SioTemperatureScanResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

// Defined in simulated_fan_writer.cpp.
std::unique_ptr<FanWriter> MakeSimulatedFanWriter(
    const RuntimeWritePolicy& runtime_policy);

// Defined in sio_fan_writer.cpp.
std::unique_ptr<FanWriter> MakeSioFanWriter(
    const RuntimeWritePolicy& runtime_policy);

}  // namespace svg_mb_control
