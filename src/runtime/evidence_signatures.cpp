#include "evidence_signatures.h"

#include <cstdint>
#include <sstream>

namespace svg_mb_control::evidence_detail {

bool DetectChanged(std::optional<std::string>& previous,
                   const std::string& current) {
    const bool changed = !previous.has_value() || *previous != current;
    previous = current;
    return changed;
}

std::string AmdSignature(const RuntimeSnapshot& snapshot) {
    std::ostringstream sig;
    sig << snapshot.amd_sensors.size();
    for (const auto& sensor : snapshot.amd_sensors) {
        sig << '|' << sensor.label << '=' << sensor.temperature_c;
    }
    return sig.str();
}

std::string GpuThermalSignature(const RuntimeSnapshot& snapshot) {
    std::ostringstream sig;
    sig << snapshot.gpu.available
        << '|' << snapshot.gpu.gpu_name
        << '|' << snapshot.gpu.last_warning
        << '|' << snapshot.gpu.core_c
        << '|' << snapshot.gpu.memjn_c
        << '|' << snapshot.gpu.hotspot_c;
    return sig.str();
}

std::string FanStateSignature(const RuntimeSnapshot& snapshot) {
    std::ostringstream sig;
    sig << snapshot.fans.size();
    for (const auto& fan : snapshot.fans) {
        sig << '|' << fan.channel
            << ':' << fan.rpm
            << ':' << fan.tach_raw
            << ':' << static_cast<unsigned int>(fan.duty_raw)
            << ':' << static_cast<unsigned int>(fan.mode_raw)
            << ':' << fan.duty_percent
            << ':' << fan.tach_valid
            << ':' << fan.manual_override
            << ':' << fan.write_allowed
            << ':' << fan.policy_blocked
            << ':' << fan.effective_write_allowed;
    }
    return sig.str();
}

std::string RuntimeSnapshotSignature(const RuntimeSnapshot& snapshot) {
    std::ostringstream sig;
    sig << snapshot.policy_writes_enabled_present
        << '|' << snapshot.policy_writes_enabled
        << "|amd:" << AmdSignature(snapshot)
        << "|gpu:" << GpuThermalSignature(snapshot)
        << "|fan:" << FanStateSignature(snapshot);
    return sig.str();
}

std::vector<RuntimeSioVoltageLogState> ConvertVoltages(
    const SioVoltageScanResult& result) {
    std::vector<RuntimeSioVoltageLogState> voltages;
    if (!result) {
        return voltages;
    }
    voltages.reserve(result.voltages.size());
    for (const auto& source : result.voltages) {
        voltages.push_back(RuntimeSioVoltageLogState{
            .index = source.index,
            .voltage_v = source.voltage_v,
            .raw = source.raw,
            .label = source.label,
        });
    }
    return voltages;
}

std::vector<RuntimeFanTachEvidenceLogState> ConvertFanTachEvidence(
    const FanTachEvidenceScanResult& result) {
    std::vector<RuntimeFanTachEvidenceLogState> fans;
    if (!result) {
        return fans;
    }
    fans.reserve(result.fans.size());
    for (const auto& source : result.fans) {
        fans.push_back(RuntimeFanTachEvidenceLogState{
            .channel = source.channel,
            .tach_hi_raw = source.tach_hi_raw,
            .tach_lo_raw = source.tach_lo_raw,
        });
    }
    return fans;
}

std::string FanTachEvidenceSignature(
    const FanTachEvidenceScanResult& result) {
    std::ostringstream sig;
    sig << result.ok() << '|' << result.fans.size();
    for (const auto& fan : result.fans) {
        sig << '|' << fan.channel
            << ':' << static_cast<unsigned int>(fan.tach_hi_raw)
            << ':' << static_cast<unsigned int>(fan.tach_lo_raw);
    }
    return sig.str();
}

std::vector<RuntimeSioTemperatureLogState> ConvertTemperatures(
    const SioTemperatureScanResult& result) {
    std::vector<RuntimeSioTemperatureLogState> temperatures;
    if (!result) {
        return temperatures;
    }
    temperatures.reserve(result.temperatures.size());
    for (const auto& source : result.temperatures) {
        temperatures.push_back(RuntimeSioTemperatureLogState{
            .index = source.index,
            .temperature_c = source.temperature_c,
            .raw = source.raw,
            .half_raw = source.half_raw,
            .valid = source.valid,
            .label = source.label,
        });
    }
    return temperatures;
}

std::string VoltageSignature(const SioVoltageScanResult& result) {
    std::ostringstream sig;
    sig << result.ok() << '|' << result.voltages.size();
    for (const auto& voltage : result.voltages) {
        sig << '|' << voltage.index
            << ':' << voltage.label
            << ':' << static_cast<unsigned int>(voltage.raw)
            << ':' << voltage.voltage_v;
    }
    return sig.str();
}

std::string TemperatureSignature(const SioTemperatureScanResult& result) {
    std::ostringstream sig;
    sig << result.ok() << '|' << result.temperatures.size();
    for (const auto& temperature : result.temperatures) {
        sig << '|' << temperature.index
            << ':' << temperature.label
            << ':' << static_cast<unsigned int>(temperature.raw)
            << ':' << static_cast<unsigned int>(temperature.half_raw)
            << ':' << temperature.valid
            << ':' << temperature.temperature_c;
    }
    return sig.str();
}

std::string GpuEvidenceSignature(const GpuEvidenceSample& sample) {
    std::ostringstream sig;
    sig << sample.available
        << '|' << sample.sample_mode
        << '|' << sample.gpu_name
        << '|' << sample.last_warning
        << '|' << sample.core_c
        << '|' << sample.memjn_c
        << '|' << sample.hotspot_c
        << '|' << sample.nvml_temp_c
        << '|' << sample.clock_graphics_mhz
        << '|' << sample.clock_memory_mhz
        << '|' << sample.clock_video_mhz
        << '|' << sample.pstate
        << '|' << sample.util_gpu_pct
        << '|' << sample.util_fb_pct
        << '|' << sample.util_vid_pct
        << '|' << sample.nvml_util_gpu_pct
        << '|' << sample.nvml_util_mem_pct
        << '|' << sample.vram_used_mb
        << '|' << sample.vram_free_mb
        << '|' << sample.vram_total_mb
        << '|' << sample.nvml_power_mw
        << '|' << sample.power_source
        << '|' << sample.voltage_core_mv
        << '|' << sample.pcie_tx_kb_s
        << '|' << sample.pcie_rx_kb_s
        << '|' << sample.throttle_reasons
        << "|fans:" << sample.fan_count;
    for (std::uint32_t index = 0u;
         index < sample.fan_count && index < kGpuEvidenceMaxFans;
         ++index) {
        sig << '|' << sample.fans[index].level_pct
            << ':' << sample.fans[index].rpm;
    }
    sig << "|rails:" << sample.power_rail_count;
    for (std::uint32_t index = 0u;
         index < sample.power_rail_count && index < kGpuEvidenceMaxPowerRails;
         ++index) {
        sig << '|' << sample.power_rails[index].domain
            << ':' << sample.power_rails[index].power_mw;
    }
    sig << "|thermal:" << sample.thermal_slot_count;
    for (std::uint32_t index = 0u;
         index < sample.thermal_slot_count && index < kGpuEvidenceMaxThermalSlots;
         ++index) {
        sig << '|' << sample.thermal_slots[index];
    }
    return sig.str();
}

std::string BuildSioEvidenceDetail(
    const FanTachEvidenceScanResult& fan_tach_result,
    const SioVoltageScanResult& voltage_result,
    const SioTemperatureScanResult& temperature_result) {
    if (fan_tach_result && voltage_result && temperature_result) {
        return "SIO fan tach, voltage, and temperature evidence captured";
    }
    std::ostringstream detail;
    if (!fan_tach_result) {
        detail << "fan tach: " << fan_tach_result.detail;
    }
    if (!voltage_result) {
        if (detail.tellp() > 0) {
            detail << "; ";
        }
        detail << "voltage: " << voltage_result.detail;
    }
    if (!temperature_result) {
        if (detail.tellp() > 0) {
            detail << "; ";
        }
        detail << "temperature: " << temperature_result.detail;
    }
    return detail.str();
}

}  // namespace svg_mb_control::evidence_detail
