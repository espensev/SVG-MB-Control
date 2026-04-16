#include "gpu_telemetry/gpu_telemetry_c.h"

#include "gpu_telemetry/gpu_sensor_reader.h"

#include <algorithm>
#include <cstring>
#include <string>

struct gpu_telemetry_reader_t {
    GpuSensorReader reader;
};

namespace {

constexpr char kUnhandledExceptionMessage[] = "Unhandled C++ exception.";

void clear_buffer(char* buffer, size_t buffer_size) {
    if (buffer != nullptr && buffer_size > 0)
        buffer[0] = '\0';
}

void copy_string_to_buffer(const std::string& value,
                           char* buffer,
                           size_t buffer_size) {
    clear_buffer(buffer, buffer_size);
    if (buffer == nullptr || buffer_size == 0)
        return;

    const size_t copy_size = std::min(value.size(), buffer_size - 1);
    if (copy_size > 0)
        std::memcpy(buffer, value.data(), copy_size);
    buffer[copy_size] = '\0';
}

template <size_t N>
void copy_fixed_string(const std::string& value, char (&buffer)[N]) {
    copy_string_to_buffer(value, buffer, N);
}

bool to_sample_mode(gpu_telemetry_sample_mode_t mode, GpuSampleMode& out_mode) {
    switch (mode) {
        case GPU_TELEMETRY_SAMPLE_MODE_THERMAL_FAST:
            out_mode = GpuSampleMode::ThermalFast;
            return true;
        case GPU_TELEMETRY_SAMPLE_MODE_FAST:
            out_mode = GpuSampleMode::Fast;
            return true;
        case GPU_TELEMETRY_SAMPLE_MODE_MEDIUM:
            out_mode = GpuSampleMode::Medium;
            return true;
        case GPU_TELEMETRY_SAMPLE_MODE_SLOW:
            out_mode = GpuSampleMode::Slow;
            return true;
        case GPU_TELEMETRY_SAMPLE_MODE_RARE:
            out_mode = GpuSampleMode::Rare;
            return true;
        case GPU_TELEMETRY_SAMPLE_MODE_FULL:
            out_mode = GpuSampleMode::Full;
            return true;
        default:
            return false;
    }
}

void copy_gpu_info(const GpuInfo& source, gpu_telemetry_gpu_info_t* target) {
    std::memset(target, 0, sizeof(*target));
    target->index = source.index;
    copy_fixed_string(source.name, target->name);
    copy_fixed_string(source.nvml_pci_bus_id, target->nvml_pci_bus_id);
    target->has_nvml = source.has_nvml ? 1 : 0;
    target->has_hotspot = source.has_hotspot ? 1 : 0;
    target->thermal.mask = source.thermal.mask;
    target->thermal.sensor_count = source.thermal.sensor_count;
    target->thermal.core_sensor_idx = source.thermal.core_sensor_idx;
    target->thermal.memjn_sensor_idx = source.thermal.memjn_sensor_idx;
}

void copy_snapshot(const GpuSnapshot& source, gpu_telemetry_snapshot_t* target) {
    std::memset(target, 0, sizeof(*target));
    target->gpu_index = source.gpu_index;
    target->time_ms = source.time_ms;
    target->dt_ms = source.dt_ms;

    target->core_c = source.core_c;
    target->memjn_c = source.memjn_c;
    target->hotspot_c = source.hotspot_c;
    target->thermal_slot_count = source.thermal_slot_count;
    std::memcpy(target->thermal_slots, source.thermal_slots,
                source.thermal_slot_count * sizeof(int32_t));
    target->nvml_temp_c = source.nvml_temp_c;

    target->clock_graphics_mhz = source.clock_graphics_mhz;
    target->clock_memory_mhz = source.clock_memory_mhz;
    target->clock_video_mhz = source.clock_video_mhz;
    target->clock_boost_mhz = source.clock_boost_mhz;

    target->nvml_clock_graphics_mhz = source.nvml_clock_graphics_mhz;
    target->nvml_clock_memory_mhz = source.nvml_clock_memory_mhz;
    target->nvml_clock_video_mhz = source.nvml_clock_video_mhz;

    target->pstate = source.pstate;

    target->util_gpu_pct = source.util_gpu_pct;
    target->util_fb_pct = source.util_fb_pct;
    target->util_vid_pct = source.util_vid_pct;

    target->nvml_util_gpu_pct = source.nvml_util_gpu_pct;
    target->nvml_util_mem_pct = source.nvml_util_mem_pct;
    target->nvml_encoder_pct = source.nvml_encoder_pct;
    target->nvml_decoder_pct = source.nvml_decoder_pct;

    target->vram_used_mb = source.vram_used_mb;
    target->vram_free_mb = source.vram_free_mb;
    target->vram_total_mb = source.vram_total_mb;

    target->fan_count = source.fan_count;
    const int fan_limit = std::min(source.fan_count, GPU_SNAPSHOT_MAX_FANS);
    for (int i = 0; i < fan_limit; ++i) {
        target->fans[i].level_pct = source.fans[i].level_pct;
        target->fans[i].rpm = source.fans[i].rpm;
    }

    target->nvml_power_mw = source.nvml_power_mw;

    target->power_rail_count = source.power_rail_count;
    const int rail_limit = std::min(source.power_rail_count, GPU_SNAPSHOT_MAX_POWER_RAILS);
    for (int i = 0; i < rail_limit; ++i) {
        target->power_rails[i].domain = source.power_rails[i].domain;
        target->power_rails[i].power_mw = source.power_rails[i].power_mw;
    }

    target->voltage_core_mv = source.voltage_core_mv;

    target->pcie_tx_kb_s = source.pcie_tx_kb_s;
    target->pcie_rx_kb_s = source.pcie_rx_kb_s;
    target->pcie_link_gen = source.pcie_link_gen;
    target->pcie_link_width = source.pcie_link_width;

    target->throttle_reasons = source.throttle_reasons;
}

}  // namespace

extern "C" {

int GPU_TELEMETRY_CALL gpu_telemetry_get_api_version(void) {
    return GPU_TELEMETRY_CAPI_VERSION;
}

gpu_telemetry_reader_t* GPU_TELEMETRY_CALL gpu_telemetry_reader_create(void) {
    try {
        return new gpu_telemetry_reader_t();
    } catch (...) {
        return nullptr;
    }
}

void GPU_TELEMETRY_CALL gpu_telemetry_reader_destroy(
    gpu_telemetry_reader_t* reader) {
    delete reader;
}

int GPU_TELEMETRY_CALL gpu_telemetry_reader_init(gpu_telemetry_reader_t* reader,
                                                 char* warning_buffer,
                                                 size_t warning_buffer_size) {
    clear_buffer(warning_buffer, warning_buffer_size);
    if (reader == nullptr)
        return 0;

    try {
        std::string warning;
        const bool ok = reader->reader.init(warning);
        copy_string_to_buffer(warning, warning_buffer, warning_buffer_size);
        return ok ? 1 : 0;
    } catch (...) {
        copy_string_to_buffer(kUnhandledExceptionMessage,
                              warning_buffer,
                              warning_buffer_size);
        return 0;
    }
}

void GPU_TELEMETRY_CALL gpu_telemetry_reader_shutdown(
    gpu_telemetry_reader_t* reader) {
    if (reader == nullptr)
        return;

    try {
        reader->reader.shutdown();
    } catch (...) {
    }
}

int GPU_TELEMETRY_CALL gpu_telemetry_reader_is_initialized(
    const gpu_telemetry_reader_t* reader) {
    if (reader == nullptr)
        return 0;

    try {
        return reader->reader.is_initialized() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int GPU_TELEMETRY_CALL gpu_telemetry_reader_get_gpu_count(
    const gpu_telemetry_reader_t* reader) {
    if (reader == nullptr)
        return 0;

    try {
        return reader->reader.gpu_count();
    } catch (...) {
        return 0;
    }
}

int GPU_TELEMETRY_CALL gpu_telemetry_reader_get_gpu_info(
    const gpu_telemetry_reader_t* reader,
    int gpu_index,
    gpu_telemetry_gpu_info_t* out_info) {
    if (out_info == nullptr)
        return 0;

    std::memset(out_info, 0, sizeof(*out_info));
    if (reader == nullptr)
        return 0;

    try {
        const GpuInfo* info = reader->reader.gpu_info(gpu_index);
        if (!info)
            return 0;

        copy_gpu_info(*info, out_info);
        return 1;
    } catch (...) {
        return 0;
    }
}

int GPU_TELEMETRY_CALL gpu_telemetry_reader_sample(
    const gpu_telemetry_reader_t* reader,
    int gpu_index,
    gpu_telemetry_sample_mode_t mode,
    gpu_telemetry_snapshot_t* out_snapshot) {
    if (out_snapshot == nullptr)
        return 0;

    std::memset(out_snapshot, 0, sizeof(*out_snapshot));
    if (reader == nullptr)
        return 0;

    try {
        GpuSampleMode cpp_mode = GpuSampleMode::Full;
        if (!to_sample_mode(mode, cpp_mode))
            return 0;

        GpuSnapshot snapshot{};
        if (!reader->reader.sample(gpu_index, snapshot, cpp_mode))
            return 0;

        copy_snapshot(snapshot, out_snapshot);
        return 1;
    } catch (...) {
        return 0;
    }
}

}  // extern "C"
