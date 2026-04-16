#ifndef NVAPI_THERMALS_H
#define NVAPI_THERMALS_H

#include "nvapi_undoc_types.h"
#include "nvapi_loader.h"

#include <string>

/*
 * Internal thermal sensor discovery result (filled by discover_sensors).
 *
 * Sensor update rates (empirically measured via spin-poll in temp_test):
 *   - Core (index 1):     ~200 Hz refresh, 0.004 C precision (Q8.8)
 *   - Mem junction:       ~2 Hz refresh, ~2 C steps
 *   - Other indices:      varies - some update slowly, some are stuck/sentinel
 *
 * The mask and index fields are precomputed at discovery and reused for every
 * read call. A single API call with the full mask returns all sensors at once
 * in ~310 us. The caller should be aware that different sensors within the
 * same response have very different refresh rates.
 */
struct NvThermalDiscovery {
    uint32_t mask                          = 0;   /* precomputed bitmask for full read      */
    int      count                         = 0;   /* highest valid sensor index + 1         */
    int      core_index                    = -1;  /* index into temperatures[] for core     */
    int      hotspot_index                 = -1;  /* undoc hotspot slot when explicitly proven */
    int      documented_hotspot_sensor_idx = -1;  /* thermal-settings hotspot sensor index   */
    int      memj_index                    = -1;  /* index into temperatures[] for mem jnct */
    int32_t  initial[NVAPI_UNDOC_THERMAL_VALUES] = {};
};

class NvApiThermals {
public:
    NvApiThermals() = default;

    bool init(const NvApiLoader& loader, std::string& out_warning);
    void shutdown();

    bool is_ready() const { return ready_; }

    /* Per-bit v2 probe to discover valid sensor indices for a GPU.
     *
     * Algorithm: call 0x65FE3AAD with struct version 2 and mask = (1 << bit)
     * for each bit 0..31. Build a sparse mask from all successful probes
     * (handles gaps between valid sensor indices).
     * Then full-read with that mask, query the documented thermal settings
     * API (NV_THERMAL_TARGET_ALL) for cross-validation, and identify:
     *   - Core: cross-validated against documented GPU sensor temp.
     *     RTX 50xx: index 1.  RTX 30xx: index 0.
     *   - Hotspot: prefer the documented thermal-settings hotspot when
     *     available. Undocumented hotspot slots are only exposed when they
     *     are explicitly proven, not inferred from a high raw reading.
     *   - Mem junction: layout-aware candidate list (50xx: {2,7,9,8},
     *     30xx/40xx: {9,8,7}), excluding indices already assigned. */
    bool discover_sensors(NvPhysicalGpuHandle gpu, NvThermalDiscovery& out) const;

    /* Fast read using precomputed mask (~310 us).
     * Returns all temperatures in Q8.8 fixed-point (divide by 256.0 for C).
     * Sentinel value 65280 = 255.0 C = invalid/absent sensor. */
    bool read(NvPhysicalGpuHandle gpu, uint32_t mask,
              int32_t out[NVAPI_UNDOC_THERMAL_VALUES]) const;

    /* Read the documented hotspot sensor exposed by thermal settings.
     * Returns Celsius directly when a cached hotspot sensor index is valid. */
    bool read_documented_hotspot_c(NvPhysicalGpuHandle gpu, int sensor_idx,
                                   double& out_c) const;

    /* Prefer the documented hotspot sensor. Use an undocumented slot only
     * when the mapping is explicitly trusted. */
    bool read_hotspot_c(NvPhysicalGpuHandle gpu, int documented_sensor_idx,
                        int fallback_index,
                        const int32_t raw[NVAPI_UNDOC_THERMAL_VALUES],
                        int raw_count, double& out_c) const;

    /* Raw sentinel value: 255.0 C in Q8.8 = 65280. Any sensor returning
     * this value is absent or invalid - filter before use. */
    static constexpr int32_t SENTINEL_RAW = 65280;

private:
    static bool read_fallback_hotspot_c(int fallback_index,
                                        const int32_t raw[NVAPI_UNDOC_THERMAL_VALUES],
                                        int raw_count, double& out_c);

    bool ready_ = false;
    NvAPI_GPU_GetThermalSensorsUndoc_t fn_get_thermal_u_ = nullptr;
    NvAPI_GPU_GetThermalSettings_t fn_get_thermal_settings_ = nullptr;
};

#endif /* NVAPI_THERMALS_H */
