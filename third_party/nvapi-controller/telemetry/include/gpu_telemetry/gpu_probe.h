#ifndef GPU_PROBE_H
#define GPU_PROBE_H

#include "gpu_snapshot.h"

#include <cstdint>
#include <memory>
#include <string>

/*
 * Stable, shareable GPU discovery metadata. Vendor handles and loader state
 * stay private to the implementation so downstream projects only depend on
 * data that is safe to expose across releases.
 */
struct GpuThermalInfo {
    uint32_t mask = 0;
    int sensor_count = 0;
    int core_sensor_idx = -1;
    int memjn_sensor_idx = -1;
};

struct GpuInfo {
    int index = -1;
    std::string name;
    std::string nvml_pci_bus_id;
    bool has_nvml = false;
    bool has_hotspot = false;
    GpuThermalInfo thermal{};
};

/*
 * Unified API probe. Loads NVAPI/NVML internally, enumerates GPUs, and
 * provides fast-path and slow-path sampling without exposing vendor state.
 */
class GpuProbe {
public:
    GpuProbe();
    ~GpuProbe();

    GpuProbe(GpuProbe&&) noexcept;
    GpuProbe& operator=(GpuProbe&&) noexcept;

    GpuProbe(const GpuProbe&) = delete;
    GpuProbe& operator=(const GpuProbe&) = delete;

    /* Load APIs and enumerate GPUs. Returns false on fatal errors. */
    bool init(std::string& out_warning);

    /* Shutdown all APIs. */
    void shutdown();

    int gpu_count() const;
    const GpuInfo& gpu(int index) const;

    /*
     * Fast-path sample: thermals, clocks, utilization, pstate.
     * Lightweight (~1ms or less).
     * Returns false only for invalid gpu_index.
     */
    bool sample_fast(int gpu_index, GpuSnapshot& snap) const;

    /*
     * Slow-path sample: fans, power, VRAM, PCIe, throttle, voltage.
     * Heavier (~5-10ms).
     * Returns false only for invalid gpu_index.
     */
    bool sample_slow(int gpu_index, GpuSnapshot& snap) const;

    /* ---- Tiered sampling for HD mode ---- */

    /* Tier 0: undoc core/memjn temp only (cheapest call).
     * Returns false only for invalid gpu_index. */
    bool sample_thermal_fast(int gpu_index, GpuSnapshot& snap) const;

    /* Tier 1: clocks, utilization, pstate, power, NVML temp.
     * Returns false only for invalid gpu_index. */
    bool sample_medium(int gpu_index, GpuSnapshot& snap) const;

    /* Rare tier: VRAM, PCIe, throttle, voltage, encoder/decoder.
     * Returns false only for invalid gpu_index. */
    bool sample_rare(int gpu_index, GpuSnapshot& snap) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif /* GPU_PROBE_H */
