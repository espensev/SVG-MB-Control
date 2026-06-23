#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace svg_mb_control {

constexpr std::size_t kGpuEvidenceMaxFans = 4u;
constexpr std::size_t kGpuEvidenceMaxPowerRails = 4u;
constexpr std::size_t kGpuEvidenceMaxThermalSlots = 32u;

struct GpuTempSample {
    bool available = false;      // true when a live sample was produced
    double core_c = 0.0;
    double memjn_c = 0.0;
    double hotspot_c = 0.0;      // 0.0 when sensor absent (e.g., RTX 5090)
    std::string gpu_name;
    std::string last_warning;

    // FEAT-0020 read-only GPU board power (logging-only; never a control input),
    // produced on the per-tick thermal sample. acquisition: "disabled" (GPU
    // telemetry not built in) | "unavailable" (no live nonzero read) | "nvml".
    // power_mw / power_time_ms are blank (NaN) unless a fresh nonzero NVML read
    // succeeded; power_sample_id advances only on such a read (a repeated id
    // marks a stale value), so no false zero is emitted.
    std::string power_acquisition = "disabled";
    std::string power_source = "unknown";
    std::uint64_t power_sample_id = 0u;
    double power_time_ms = std::numeric_limits<double>::quiet_NaN();
    double power_mw = std::numeric_limits<double>::quiet_NaN();

    // FEAT-0021 read-only GPU workload context. This is a cached, logging-only
    // sample beside board power; it is never a control input. acquisition:
    // "disabled" | "unavailable" | "nvml". sample_age_ms shows how old the
    // cached context is when mirrored into a control-loop row.
    std::string context_acquisition = "disabled";
    std::uint64_t context_sample_id = 0u;
    double context_time_ms = std::numeric_limits<double>::quiet_NaN();
    double context_sample_age_ms = std::numeric_limits<double>::quiet_NaN();
    std::int32_t context_util_gpu_pct = -1;
    std::int32_t context_util_mem_pct = -1;
    std::int32_t context_pstate = -1;
    std::uint32_t context_clock_graphics_mhz = 0u;
    std::uint32_t context_clock_memory_mhz = 0u;
    std::uint32_t context_vram_used_mb = 0u;
    std::uint32_t context_vram_total_mb = 0u;
};

struct GpuEvidenceFanState {
    std::uint32_t level_pct = 0u;
    std::uint32_t rpm = 0u;
};

struct GpuEvidencePowerRailState {
    std::uint32_t domain = 0u;
    std::uint32_t power_mw = 0u;
};

struct GpuEvidenceSample {
    bool available = false;
    std::string sample_mode = "full";
    std::string requested_sample_mode;
    std::string gpu_name;
    std::string detail;
    std::string last_warning;

    int gpu_index = 0;
    std::int64_t time_ms = 0;
    std::int64_t dt_ms = 0;

    double core_c = 0.0;
    double memjn_c = 0.0;
    double hotspot_c = 0.0;
    std::uint32_t nvml_temp_c = 0u;

    std::uint32_t clock_graphics_mhz = 0u;
    std::uint32_t clock_memory_mhz = 0u;
    std::uint32_t clock_video_mhz = 0u;
    std::uint32_t clock_boost_mhz = 0u;
    std::uint32_t nvml_clock_graphics_mhz = 0u;
    std::uint32_t nvml_clock_memory_mhz = 0u;
    std::uint32_t nvml_clock_video_mhz = 0u;

    std::int32_t pstate = -1;
    std::int32_t util_gpu_pct = -1;
    std::int32_t util_fb_pct = -1;
    std::int32_t util_vid_pct = -1;
    std::uint32_t nvml_util_gpu_pct = 0u;
    std::uint32_t nvml_util_mem_pct = 0u;
    std::uint32_t nvml_encoder_pct = 0u;
    std::uint32_t nvml_decoder_pct = 0u;

    std::uint32_t vram_used_mb = 0u;
    std::uint32_t vram_free_mb = 0u;
    std::uint32_t vram_total_mb = 0u;

    std::uint32_t fan_count = 0u;
    std::array<GpuEvidenceFanState, kGpuEvidenceMaxFans> fans{};

    std::uint32_t nvml_power_mw = 0u;
    std::string power_source = "unknown";
    std::uint32_t power_rail_count = 0u;
    std::array<GpuEvidencePowerRailState, kGpuEvidenceMaxPowerRails>
        power_rails{};

    std::uint32_t voltage_core_mv = 0u;
    std::uint32_t pcie_tx_kb_s = 0u;
    std::uint32_t pcie_rx_kb_s = 0u;
    std::uint32_t pcie_link_gen = 0u;
    std::uint32_t pcie_link_width = 0u;
    std::uint64_t throttle_reasons = 0u;

    std::uint32_t thermal_slot_count = 0u;
    std::array<std::int32_t, kGpuEvidenceMaxThermalSlots> thermal_slots{};
};

// RAII wrapper around the nvapi-controller `GpuSensorReader`. Reads GPU
// core/memjn/hotspot temperatures from NVAPI. Construction never throws;
// failure to initialize leaves `available()` false and subsequent
// `Sample()` calls return an empty sample with `available=false`.
class GpuReader {
  public:
    GpuReader();
    ~GpuReader();

    GpuReader(const GpuReader&) = delete;
    GpuReader& operator=(const GpuReader&) = delete;

    // Returns true when at least one GPU is enumerated and initialization
    // succeeded.
    bool available() const;

    // Diagnostic message from initialization. Empty on clean init.
    std::string init_warning() const;

    // Samples GPU 0 (first enumerated GPU). Returns a reference to an
    // internally held sample so steady-state callers do not allocate a
    // GpuTempSample (and its inner strings) per call; subsequent Sample()
    // calls overwrite the held buffer. On any failure the held sample has
    // `available=false` and `last_warning` populated.
    const GpuTempSample& Sample();

    // Samples GPU 0 using the requested evidence tier: thermal-fast, fast,
    // medium, slow, rare, or full. This is intentionally separate from
    // `Sample()` so controller/read-loop callers keep their cheap thermal
    // path.
    GpuEvidenceSample SampleEvidence(std::string_view mode_label);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace svg_mb_control
