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
    csv << ',';
    AppendCsvBool(csv, sample.available);
    csv << ',';
    AppendCsvString(csv, sample.sample_mode);
    csv << ',';
    AppendCsvString(csv, sample.requested_sample_mode);
    csv << ',';
    AppendCsvString(csv, sample.detail);
    csv << ',';
    AppendCsvString(csv, sample.gpu_name);
    csv << ',';
    AppendCsvString(csv, sample.last_warning);
    csv << ',';
    if (sample.available) {
        csv << sample.gpu_index;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.time_ms;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.dt_ms;
    }
    csv << ',';
    if (sample.available) {
        AppendCsvDouble(csv, sample.core_c);
    }
    csv << ',';
    if (sample.available) {
        AppendCsvDouble(csv, sample.memjn_c);
    }
    csv << ',';
    if (sample.available) {
        AppendCsvDouble(csv, sample.hotspot_c);
    }
    csv << ',';
    if (sample.available) {
        csv << sample.nvml_temp_c;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.clock_graphics_mhz;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.clock_memory_mhz;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.clock_video_mhz;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.clock_boost_mhz;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.nvml_clock_graphics_mhz;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.nvml_clock_memory_mhz;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.nvml_clock_video_mhz;
    }
    csv << ',';
    if (sample.available) {
        AppendCsvInt32WhenAvailable(csv, sample.pstate);
    }
    csv << ',';
    if (sample.available) {
        AppendCsvInt32WhenAvailable(csv, sample.util_gpu_pct);
    }
    csv << ',';
    if (sample.available) {
        AppendCsvInt32WhenAvailable(csv, sample.util_fb_pct);
    }
    csv << ',';
    if (sample.available) {
        AppendCsvInt32WhenAvailable(csv, sample.util_vid_pct);
    }
    csv << ',';
    if (sample.available) {
        csv << sample.nvml_util_gpu_pct;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.nvml_util_mem_pct;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.nvml_encoder_pct;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.nvml_decoder_pct;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.vram_used_mb;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.vram_free_mb;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.vram_total_mb;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.nvml_power_mw;
    }
    csv << ',';
    if (sample.available) {
        AppendCsvString(csv, sample.power_source);
    }
    csv << ',';
    if (sample.available) {
        csv << sample.voltage_core_mv;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.pcie_tx_kb_s;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.pcie_rx_kb_s;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.pcie_link_gen;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.pcie_link_width;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.throttle_reasons;
    }
    csv << ',';
    if (sample.available) {
        csv << sample.fan_count;
    }
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kGpuEvidenceMaxFans);
         ++index) {
        csv << ',';
        if (sample.available && index < sample.fan_count) {
            csv << sample.fans[index].level_pct;
        }
        csv << ',';
        if (sample.available && index < sample.fan_count) {
            csv << sample.fans[index].rpm;
        }
    }
    csv << ',';
    if (sample.available) {
        csv << sample.power_rail_count;
    }
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kGpuEvidenceMaxPowerRails);
         ++index) {
        csv << ',';
        if (sample.available && index < sample.power_rail_count) {
            csv << sample.power_rails[index].domain;
        }
        csv << ',';
        if (sample.available && index < sample.power_rail_count) {
            csv << sample.power_rails[index].power_mw;
        }
    }
    csv << ',';
    if (sample.available) {
        csv << sample.thermal_slot_count;
    }
    for (std::uint32_t index = 0u;
         index < static_cast<std::uint32_t>(kGpuEvidenceMaxThermalSlots);
         ++index) {
        csv << ',';
        if (sample.available && index < sample.thermal_slot_count) {
            csv << sample.thermal_slots[index];
        }
    }
}

}  // namespace svg_mb_control
