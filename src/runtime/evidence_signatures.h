#pragma once

#include "fan_writer.h"
#include "gpu_reader.h"
#include "runtime_csv_rows.h"
#include "runtime_snapshot.h"

#include <optional>
#include <string>
#include <vector>

namespace svg_mb_control::evidence_detail {

// Pure change-detection / signature helpers for the evidence-log poll loop.
// Each is a stateless transform over a snapshot or scan-result struct; none
// touch the loop's runtime_home / CSV-logger state. Split out of
// evidence_log.cpp so they can be reasoned about and unit-tested in isolation.

bool DetectChanged(std::optional<std::string>& previous,
                   const std::string& current);

std::string AmdSignature(const RuntimeSnapshot& snapshot);
std::string GpuThermalSignature(const RuntimeSnapshot& snapshot);
std::string FanStateSignature(const RuntimeSnapshot& snapshot);
std::string RuntimeSnapshotSignature(const RuntimeSnapshot& snapshot);

std::vector<RuntimeSioVoltageLogState> ConvertVoltages(
    const SioVoltageScanResult& result);
std::vector<RuntimeFanTachEvidenceLogState> ConvertFanTachEvidence(
    const FanTachEvidenceScanResult& result);
std::vector<RuntimeSioTemperatureLogState> ConvertTemperatures(
    const SioTemperatureScanResult& result);

std::string FanTachEvidenceSignature(const FanTachEvidenceScanResult& result);
std::string VoltageSignature(const SioVoltageScanResult& result);
std::string TemperatureSignature(const SioTemperatureScanResult& result);
std::string GpuEvidenceSignature(const GpuEvidenceSample& sample);

std::string BuildSioEvidenceDetail(
    const FanTachEvidenceScanResult& fan_tach_result,
    const SioVoltageScanResult& voltage_result,
    const SioTemperatureScanResult& temperature_result);

}  // namespace svg_mb_control::evidence_detail
