#ifndef GPU_SENSOR_READER_H
#define GPU_SENSOR_READER_H

#include "gpu_probe.h"

#include <vector>

/*
 * Convenience sampling profiles for downstream apps that want a simpler
 * sensor-reader API than the lower-level GpuProbe tiers.
 */
enum class GpuSampleMode {
    ThermalFast,
    Fast,
    Medium,
    Slow,
    Rare,
    Full
};

/*
 * Reusable sensor-reader facade for applications that need GPU telemetry
 * without owning the logger app's polling logic. Full samples combine the
 * fast and slow probe paths to populate one GpuSnapshot per GPU.
 */
class GpuSensorReader {
public:
    GpuSensorReader();
    ~GpuSensorReader();

    GpuSensorReader(GpuSensorReader&&) noexcept;
    GpuSensorReader& operator=(GpuSensorReader&&) noexcept;

    GpuSensorReader(const GpuSensorReader&) = delete;
    GpuSensorReader& operator=(const GpuSensorReader&) = delete;

    bool init(std::string& out_warning);
    void shutdown();

    bool is_initialized() const;

    int gpu_count() const;
    std::vector<GpuInfo> gpu_info() const;
    const GpuInfo* gpu_info(int index) const;

    /*
     * Returns false when the reader is not initialized or gpu_index is out of
     * range. The snapshot is cleared before each sample attempt.
     */
    bool sample(int gpu_index, GpuSnapshot& out_snapshot,
                GpuSampleMode mode = GpuSampleMode::Full) const;

    /*
     * Returns one snapshot per enumerated GPU. Each entry starts zeroed.
     */
    std::vector<GpuSnapshot> sample_all(
        GpuSampleMode mode = GpuSampleMode::Full) const;

private:
    GpuProbe probe_;
    bool initialized_ = false;
};

#endif /* GPU_SENSOR_READER_H */
