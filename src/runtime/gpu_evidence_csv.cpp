#include "gpu_evidence_csv.h"

#include "csv_util.h"

#include <cstdint>

namespace svg_mb_control {

std::string BuildGpuEvidenceCsvHeader() {
    std::ostringstream header;
    header << ",gpu_evidence_available"
           << ",gpu_evidence_sample_mode"
           << ",gpu_evidence_requested_sample_mode"
           << ",gpu_evidence_detail"
           << ",gpu_evidence_gpu_name"
           << ",gpu_evidence_last_warning"
           << ",gpu_evidence_index"
           << ",gpu_evidence_time_ms"
           << ",gpu_evidence_dt_ms"
           << ",gpu_evidence_core_c"
           << ",gpu_evidence_memjn_c"
           << ",gpu_evidence_hotspot_c"
           << ",gpu_evidence_nvml_temp_c"
           << ",gpu_evidence_clock_graphics_mhz"
           << ",gpu_evidence_clock_memory_mhz"
           << ",gpu_evidence_clock_video_mhz"
           << ",gpu_evidence_clock_boost_mhz"
           << ",gpu_evidence_nvml_clock_graphics_mhz"
           << ",gpu_evidence_nvml_clock_memory_mhz"
           << ",gpu_evidence_nvml_clock_video_mhz"
           << ",gpu_evidence_pstate"
           << ",gpu_evidence_util_gpu_pct"
           << ",gpu_evidence_util_fb_pct"
           << ",gpu_evidence_util_vid_pct"
           << ",gpu_evidence_nvml_util_gpu_pct"
           << ",gpu_evidence_nvml_util_mem_pct"
           << ",gpu_evidence_nvml_encoder_pct"
           << ",gpu_evidence_nvml_decoder_pct"
           << ",gpu_evidence_vram_used_mb"
           << ",gpu_evidence_vram_free_mb"
           << ",gpu_evidence_vram_total_mb"
           << ",gpu_evidence_nvml_power_mw"
           << ",gpu_evidence_power_source"
           << ",gpu_evidence_voltage_core_mv"
           << ",gpu_evidence_pcie_tx_kb_s"
           << ",gpu_evidence_pcie_rx_kb_s"
           << ",gpu_evidence_pcie_link_gen"
           << ",gpu_evidence_pcie_link_width"
           << ",gpu_evidence_throttle_reasons"
           << ",gpu_evidence_fan_count";
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kGpuEvidenceMaxFans);
         ++index) {
        header << ",gpu_evidence_fan" << index << "_level_pct"
               << ",gpu_evidence_fan" << index << "_rpm";
    }
    header << ",gpu_evidence_power_rail_count";
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kGpuEvidenceMaxPowerRails);
         ++index) {
        header << ",gpu_evidence_power_rail" << index << "_domain"
               << ",gpu_evidence_power_rail" << index << "_power_mw";
    }
    header << ",gpu_evidence_thermal_slot_count";
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kGpuEvidenceMaxThermalSlots);
         ++index) {
        header << ",gpu_evidence_thermal_slot" << index << "_raw";
    }
    return header.str();
}

void AppendGpuEvidenceCsvRow(std::ostringstream& csv,
                             const GpuEvidenceSample& sample) {
    const bool ok = sample.available;
    AppendCsvFieldBool(csv, ok);
    AppendCsvFieldString(csv, sample.sample_mode);
    AppendCsvFieldString(csv, sample.requested_sample_mode);
    AppendCsvFieldString(csv, sample.detail);
    AppendCsvFieldString(csv, sample.gpu_name);
    AppendCsvFieldString(csv, sample.last_warning);
    AppendCsvFieldIf(csv, ok, sample.gpu_index);
    AppendCsvFieldIf(csv, ok, sample.time_ms);
    AppendCsvFieldIf(csv, ok, sample.dt_ms);
    AppendCsvFieldDoubleIf(csv, ok, sample.core_c);
    AppendCsvFieldDoubleIf(csv, ok, sample.memjn_c);
    AppendCsvFieldDoubleIf(csv, ok, sample.hotspot_c);
    AppendCsvFieldIf(csv, ok, sample.nvml_temp_c);
    AppendCsvFieldIf(csv, ok, sample.clock_graphics_mhz);
    AppendCsvFieldIf(csv, ok, sample.clock_memory_mhz);
    AppendCsvFieldIf(csv, ok, sample.clock_video_mhz);
    AppendCsvFieldIf(csv, ok, sample.clock_boost_mhz);
    AppendCsvFieldIf(csv, ok, sample.nvml_clock_graphics_mhz);
    AppendCsvFieldIf(csv, ok, sample.nvml_clock_memory_mhz);
    AppendCsvFieldIf(csv, ok, sample.nvml_clock_video_mhz);
    AppendCsvFieldInt32IfAvailable(csv, ok, sample.pstate);
    AppendCsvFieldInt32IfAvailable(csv, ok, sample.util_gpu_pct);
    AppendCsvFieldInt32IfAvailable(csv, ok, sample.util_fb_pct);
    AppendCsvFieldInt32IfAvailable(csv, ok, sample.util_vid_pct);
    AppendCsvFieldIf(csv, ok, sample.nvml_util_gpu_pct);
    AppendCsvFieldIf(csv, ok, sample.nvml_util_mem_pct);
    AppendCsvFieldIf(csv, ok, sample.nvml_encoder_pct);
    AppendCsvFieldIf(csv, ok, sample.nvml_decoder_pct);
    AppendCsvFieldIf(csv, ok, sample.vram_used_mb);
    AppendCsvFieldIf(csv, ok, sample.vram_free_mb);
    AppendCsvFieldIf(csv, ok, sample.vram_total_mb);
    AppendCsvFieldIf(csv, ok, sample.nvml_power_mw);
    AppendCsvFieldStringIf(csv, ok, sample.power_source);
    AppendCsvFieldIf(csv, ok, sample.voltage_core_mv);
    AppendCsvFieldIf(csv, ok, sample.pcie_tx_kb_s);
    AppendCsvFieldIf(csv, ok, sample.pcie_rx_kb_s);
    AppendCsvFieldIf(csv, ok, sample.pcie_link_gen);
    AppendCsvFieldIf(csv, ok, sample.pcie_link_width);
    AppendCsvFieldIf(csv, ok, sample.throttle_reasons);
    AppendCsvFieldIf(csv, ok, sample.fan_count);
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kGpuEvidenceMaxFans);
         ++index) {
        const bool fan_ok = ok && index < sample.fan_count;
        AppendCsvFieldIf(csv, fan_ok, sample.fans[index].level_pct);
        AppendCsvFieldIf(csv, fan_ok, sample.fans[index].rpm);
    }
    AppendCsvFieldIf(csv, ok, sample.power_rail_count);
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kGpuEvidenceMaxPowerRails);
         ++index) {
        const bool rail_ok = ok && index < sample.power_rail_count;
        AppendCsvFieldIf(csv, rail_ok, sample.power_rails[index].domain);
        AppendCsvFieldIf(csv, rail_ok, sample.power_rails[index].power_mw);
    }
    AppendCsvFieldIf(csv, ok, sample.thermal_slot_count);
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kGpuEvidenceMaxThermalSlots);
         ++index) {
        const bool slot_ok = ok && index < sample.thermal_slot_count;
        AppendCsvFieldIf(csv, slot_ok, sample.thermal_slots[index]);
    }
}

}  // namespace svg_mb_control
