#ifndef GPU_TELEMETRY_C_H
#define GPU_TELEMETRY_C_H

#ifdef __cplusplus
#  include <cstddef>
#  include <cstdint>
#else
#  include <stddef.h>
#  include <stdint.h>
#endif

#if defined(_WIN32)
#  if defined(GPU_TELEMETRY_CAPI_BUILD)
#    define GPU_TELEMETRY_CAPI __declspec(dllexport)
#  elif defined(GPU_TELEMETRY_CAPI_STATIC)
#    define GPU_TELEMETRY_CAPI
#  else
#    define GPU_TELEMETRY_CAPI __declspec(dllimport)
#  endif
#  define GPU_TELEMETRY_CALL __cdecl
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define GPU_TELEMETRY_CAPI __attribute__((visibility("default")))
#  define GPU_TELEMETRY_CALL
#else
#  define GPU_TELEMETRY_CAPI
#  define GPU_TELEMETRY_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define GPU_TELEMETRY_CAPI_VERSION 2
#define GPU_TELEMETRY_MAX_FANS 4
#define GPU_TELEMETRY_MAX_POWER_RAILS 4
#define GPU_TELEMETRY_MAX_THERMAL_SLOTS 32
#define GPU_TELEMETRY_GPU_NAME_LENGTH 64
#define GPU_TELEMETRY_PCI_BUS_ID_LENGTH 32

typedef struct gpu_telemetry_reader_t gpu_telemetry_reader_t;

typedef enum gpu_telemetry_sample_mode_t {
    GPU_TELEMETRY_SAMPLE_MODE_THERMAL_FAST = 0,
    GPU_TELEMETRY_SAMPLE_MODE_FAST = 1,
    GPU_TELEMETRY_SAMPLE_MODE_MEDIUM = 2,
    GPU_TELEMETRY_SAMPLE_MODE_SLOW = 3,
    GPU_TELEMETRY_SAMPLE_MODE_RARE = 4,
    GPU_TELEMETRY_SAMPLE_MODE_FULL = 5
} gpu_telemetry_sample_mode_t;

typedef struct gpu_telemetry_thermal_info_t {
    uint32_t mask;
    int32_t sensor_count;
    int32_t core_sensor_idx;
    int32_t memjn_sensor_idx;
} gpu_telemetry_thermal_info_t;

typedef struct gpu_telemetry_gpu_info_t {
    int32_t index;
    char name[GPU_TELEMETRY_GPU_NAME_LENGTH];
    char nvml_pci_bus_id[GPU_TELEMETRY_PCI_BUS_ID_LENGTH];
    int32_t has_nvml;
    int32_t has_hotspot;
    gpu_telemetry_thermal_info_t thermal;
} gpu_telemetry_gpu_info_t;

typedef struct gpu_telemetry_fan_entry_t {
    uint32_t level_pct;
    uint32_t rpm;
} gpu_telemetry_fan_entry_t;

typedef struct gpu_telemetry_power_rail_entry_t {
    uint32_t domain;
    uint32_t power_mw;
} gpu_telemetry_power_rail_entry_t;

typedef struct gpu_telemetry_snapshot_t {
    int32_t gpu_index;
    int64_t time_ms;
    int64_t dt_ms;

    double core_c;
    double memjn_c;
    double hotspot_c;

    int32_t thermal_slot_count;
    int32_t thermal_slots[GPU_TELEMETRY_MAX_THERMAL_SLOTS];

    uint32_t nvml_temp_c;

    uint32_t clock_graphics_mhz;
    uint32_t clock_memory_mhz;
    uint32_t clock_video_mhz;
    uint32_t clock_boost_mhz;

    uint32_t nvml_clock_graphics_mhz;
    uint32_t nvml_clock_memory_mhz;
    uint32_t nvml_clock_video_mhz;

    int32_t pstate;

    int32_t util_gpu_pct;
    int32_t util_fb_pct;
    int32_t util_vid_pct;

    uint32_t nvml_util_gpu_pct;
    uint32_t nvml_util_mem_pct;
    uint32_t nvml_encoder_pct;
    uint32_t nvml_decoder_pct;

    uint32_t vram_used_mb;
    uint32_t vram_free_mb;
    uint32_t vram_total_mb;

    int32_t fan_count;
    gpu_telemetry_fan_entry_t fans[GPU_TELEMETRY_MAX_FANS];

    uint32_t nvml_power_mw;

    int32_t power_rail_count;
    gpu_telemetry_power_rail_entry_t power_rails[GPU_TELEMETRY_MAX_POWER_RAILS];

    uint32_t voltage_core_mv;

    uint32_t pcie_tx_kb_s;
    uint32_t pcie_rx_kb_s;
    uint32_t pcie_link_gen;
    uint32_t pcie_link_width;

    uint64_t throttle_reasons;
} gpu_telemetry_snapshot_t;

/* Returns the runtime C API version (GPU_TELEMETRY_CAPI_VERSION at build time).
 * Callers can compare against the compile-time define to detect mismatches. */
GPU_TELEMETRY_CAPI int GPU_TELEMETRY_CALL
gpu_telemetry_get_api_version(void);

/* Create/destroy an opaque reader instance for the C ABI. */
GPU_TELEMETRY_CAPI gpu_telemetry_reader_t* GPU_TELEMETRY_CALL
gpu_telemetry_reader_create(void);

GPU_TELEMETRY_CAPI void GPU_TELEMETRY_CALL
gpu_telemetry_reader_destroy(gpu_telemetry_reader_t* reader);

/* Initialize NVAPI/NVML. Returns 1 on success, 0 on fatal failure. */
GPU_TELEMETRY_CAPI int GPU_TELEMETRY_CALL
gpu_telemetry_reader_init(gpu_telemetry_reader_t* reader,
                          char* warning_buffer,
                          size_t warning_buffer_size);

GPU_TELEMETRY_CAPI void GPU_TELEMETRY_CALL
gpu_telemetry_reader_shutdown(gpu_telemetry_reader_t* reader);

GPU_TELEMETRY_CAPI int GPU_TELEMETRY_CALL
gpu_telemetry_reader_is_initialized(const gpu_telemetry_reader_t* reader);

GPU_TELEMETRY_CAPI int GPU_TELEMETRY_CALL
gpu_telemetry_reader_get_gpu_count(const gpu_telemetry_reader_t* reader);

GPU_TELEMETRY_CAPI int GPU_TELEMETRY_CALL
gpu_telemetry_reader_get_gpu_info(const gpu_telemetry_reader_t* reader,
                                  int gpu_index,
                                  gpu_telemetry_gpu_info_t* out_info);

/* Returns 1 when a fresh sample was produced. Returns 0 for invalid inputs,
 * uninitialized readers, or unsupported modes. */
GPU_TELEMETRY_CAPI int GPU_TELEMETRY_CALL
gpu_telemetry_reader_sample(const gpu_telemetry_reader_t* reader,
                            int gpu_index,
                            gpu_telemetry_sample_mode_t mode,
                            gpu_telemetry_snapshot_t* out_snapshot);

#ifdef __cplusplus
}
#endif

#endif /* GPU_TELEMETRY_C_H */
