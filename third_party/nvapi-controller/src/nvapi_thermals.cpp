#include "nvapi_thermals.h"

#include <algorithm>
#include <cstring>

bool NvApiThermals::init(const NvApiLoader& loader, std::string& out_warning) {
    if (!loader.is_ready()) {
        out_warning = "NVAPI loader is not ready.";
        return false;
    }

    fn_get_thermal_u_ = loader.resolve<NvAPI_GPU_GetThermalSensorsUndoc_t>(
        NVAPI_ID_GPU_GET_THERMAL_SENSORS_UNDOC);
    fn_get_thermal_settings_ = loader.resolve<NvAPI_GPU_GetThermalSettings_t>(
        NVAPI_ID_GPU_GET_THERMAL_SETTINGS);

    if (!fn_get_thermal_u_) {
        out_warning = "Cannot resolve undocumented thermal sensor function (0x65FE3AAD).";
        shutdown();
        return false;
    }

    ready_ = true;
    return true;
}

void NvApiThermals::shutdown() {
    ready_ = false;
    fn_get_thermal_u_ = nullptr;
    fn_get_thermal_settings_ = nullptr;
}

bool NvApiThermals::discover_sensors(NvPhysicalGpuHandle gpu, NvThermalDiscovery& out) const {
    if (!ready_ || !fn_get_thermal_u_) {
        return false;
    }

    out = {};

    /* Phase 1: Per-bit probe to find valid sensor indices.
     * Version 2 struct with mask = single bit. Probe all 32 bits
     * to handle sparse sensor layouts (gaps between valid indices). */
    int max_bit = 0;
    for (int bit = 0; bit < 32; bit++) {
        NvUndocThermalSensors probe;
        std::memset(&probe, 0, sizeof(probe));
        probe.version = nvapi_make_version<NvUndocThermalSensors>(2);
        probe.mask = 1u << bit;
        if (fn_get_thermal_u_(gpu, &probe) == NVAPI_OK) {
            out.mask |= (1u << bit);
            max_bit = bit + 1;
        }
    }

    if (out.mask == 0) {
        return false;
    }

    out.count = max_bit;

    /* Phase 2: Full read with discovered mask to get initial values */
    NvUndocThermalSensors us;
    std::memset(&us, 0, sizeof(us));
    us.version = nvapi_make_version<NvUndocThermalSensors>(2);
    us.mask = out.mask;

    if (fn_get_thermal_u_(gpu, &us) != NVAPI_OK) {
        out = {};
        return false;
    }

    std::memcpy(out.initial, us.temperatures, sizeof(out.initial));

    /* Phase 3: Query documented thermal settings for cross-validation.
     * The documented API gives us ground-truth sensor types (GPU, HOTSPOT)
     * that we use to correctly map undocumented sensor indices. */
    double doc_core_c = 0.0;

    if (fn_get_thermal_settings_) {
        NvGpuThermalSettings ts;
        std::memset(&ts, 0, sizeof(ts));
        ts.version = nvapi_make_version<NvGpuThermalSettings>(2);
        if (fn_get_thermal_settings_(gpu, NV_THERMAL_TARGET_ALL, &ts) == NVAPI_OK) {
            uint32_t count = std::min(
                ts.count, static_cast<uint32_t>(NVAPI_MAX_THERMAL_SETTINGS_SENSORS));
            for (uint32_t i = 0; i < count; ++i) {
                if (ts.sensor[i].target == NV_THERMAL_TARGET_GPU
                    && ts.sensor[i].current_temp > 0) {
                    doc_core_c = static_cast<double>(ts.sensor[i].current_temp);
                }
                if (ts.sensor[i].target == NV_THERMAL_TARGET_HOTSPOT
                    && ts.sensor[i].current_temp > 0) {
                    out.documented_hotspot_sensor_idx = static_cast<int>(i);
                }
            }
        }
    }

    /* Phase 4: Identify sensor roles.
     * Index 1 is the primary fast undocumented sensor, but its meaning
     * varies by generation. Cross-validate against the documented core
     * temp to decide the core mapping, but do not auto-publish index 1 as
     * hotspot: some boards expose a limit-style sensor there instead. */
    bool prefer_legacy_memj_layout = false;

    if (max_bit > 1 && (out.mask & (1u << 1))
        && us.temperatures[1] != 0 && us.temperatures[1] != SENTINEL_RAW) {
        double idx1_c = us.temperatures[1] / 256.0;
        double delta = (idx1_c > doc_core_c)
                     ? (idx1_c - doc_core_c) : (doc_core_c - idx1_c);

        if (doc_core_c > 0.0 && delta > 5.0) {
            /* Index 1 doesn't match documented core. Use that as a layout hint
             * for core/mem junction mapping, but leave hotspot on the
             * documented path unless we have an explicit undoc mapping. */
            prefer_legacy_memj_layout = true;

            double best_delta = 999.0;
            for (int idx = 0; idx < max_bit; ++idx) {
                if (idx == 1) continue;
                if (!(out.mask & (1u << idx))) continue;
                if (us.temperatures[idx] == 0 || us.temperatures[idx] == SENTINEL_RAW)
                    continue;
                double c = us.temperatures[idx] / 256.0;
                double d = (c > doc_core_c) ? (c - doc_core_c) : (doc_core_c - c);
                if (d < best_delta) {
                    best_delta = d;
                    out.core_index = idx;
                }
            }
        } else {
            /* Index 1 matches documented core, or no documented temp available. */
            out.core_index = 1;
        }
    }

    /* Memory junction: generation-dependent index.
     * RTX 50xx: idx 2, RTX 40xx: idx 7, RTX 30xx: idx 9 or 8.
     * If index 1 didn't match documented core, treat that as an older-layout
     * hint for mem junction selection, but do not expose it as hotspot. */
    static const int mem_new[] = { 2, 7, 9, 8 };   /* 50xx layout */
    static const int mem_old[] = { 9, 8, 7 };       /* 30xx/40xx layout */
    const bool older_layout = prefer_legacy_memj_layout;
    const int* candidates = older_layout ? mem_old : mem_new;
    const int  n_candidates = older_layout ? 3 : 4;

    for (int ci = 0; ci < n_candidates; ++ci) {
        int idx = candidates[ci];
        if (idx == out.core_index || idx == out.hotspot_index) continue;
        if (idx < max_bit
            && (out.mask & (1u << idx))
            && us.temperatures[idx] != 0
            && us.temperatures[idx] != SENTINEL_RAW) {
            out.memj_index = idx;
            break;
        }
    }

    return true;
}

bool NvApiThermals::read(NvPhysicalGpuHandle gpu, uint32_t mask,
                          int32_t out[NVAPI_UNDOC_THERMAL_VALUES]) const {
    if (!ready_ || !fn_get_thermal_u_ || mask == 0) {
        return false;
    }

    NvUndocThermalSensors us;
    std::memset(&us, 0, sizeof(us));
    us.version = nvapi_make_version<NvUndocThermalSensors>(2);
    us.mask = mask;

    if (fn_get_thermal_u_(gpu, &us) != NVAPI_OK) {
        return false;
    }

    std::memcpy(out, us.temperatures, sizeof(us.temperatures));
    return true;
}

bool NvApiThermals::read_documented_hotspot_c(NvPhysicalGpuHandle gpu, int sensor_idx,
                                              double& out_c) const {
    out_c = 0.0;
    if (!ready_ || !fn_get_thermal_settings_ || sensor_idx < 0) {
        return false;
    }

    NvGpuThermalSettings ts;
    std::memset(&ts, 0, sizeof(ts));
    ts.version = nvapi_make_version<NvGpuThermalSettings>(2);

    if (fn_get_thermal_settings_(gpu, NV_THERMAL_TARGET_ALL, &ts) != NVAPI_OK) {
        return false;
    }

    if (sensor_idx >= static_cast<int>(ts.count)
        || sensor_idx >= NVAPI_MAX_THERMAL_SETTINGS_SENSORS) {
        return false;
    }

    const auto& sensor = ts.sensor[sensor_idx];
    if (sensor.target != NV_THERMAL_TARGET_HOTSPOT || sensor.current_temp <= 0) {
        return false;
    }

    out_c = static_cast<double>(sensor.current_temp);
    return true;
}

bool NvApiThermals::read_hotspot_c(NvPhysicalGpuHandle gpu, int documented_sensor_idx,
                                   int fallback_index,
                                   const int32_t raw[NVAPI_UNDOC_THERMAL_VALUES],
                                   int raw_count, double& out_c) const {
    if (read_documented_hotspot_c(gpu, documented_sensor_idx, out_c)) {
        return true;
    }

    return read_fallback_hotspot_c(fallback_index, raw, raw_count, out_c);
}

bool NvApiThermals::read_fallback_hotspot_c(
    int fallback_index,
    const int32_t raw[NVAPI_UNDOC_THERMAL_VALUES],
    int raw_count,
    double& out_c) {
    out_c = 0.0;
    if (fallback_index < 0 || fallback_index >= raw_count) {
        return false;
    }

    int32_t raw_value = raw[fallback_index];
    if (raw_value == 0 || raw_value == SENTINEL_RAW) {
        return false;
    }

    out_c = raw_value / 256.0;
    return true;
}
