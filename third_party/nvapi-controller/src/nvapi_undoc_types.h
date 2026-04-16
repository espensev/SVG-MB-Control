#ifndef NVAPI_UNDOC_TYPES_H
#define NVAPI_UNDOC_TYPES_H

#include "nvapi_undoc_ids.h"

#include <cstdint>

/* ---- handles & status ---- */

typedef void* NvPhysicalGpuHandle;
typedef int32_t NvAPI_Status;

constexpr NvAPI_Status NVAPI_OK = 0;

/* ---- fan control mode ---- */

enum NvFanControlMode : uint32_t {
    NV_FAN_AUTO = 0,
    NV_FAN_MANUAL = 1
};

/* ---- fan cooler control (get/set) ---- */

struct NvFanCoolerControlItem {
    uint32_t cooler_id;
    uint32_t level; /* percent 0-100 */
    NvFanControlMode control_mode;
    uint32_t _reserved[NVAPI_FAN_CONTROL_ITEM_RESERVED];
};

struct NvFanCoolerControl {
    uint32_t version;
    uint32_t _reserved1;
    uint32_t count;
    uint32_t _reserved2[NVAPI_FAN_CONTROL_RESERVED2];
    NvFanCoolerControlItem items[NVAPI_MAX_FAN_CONTROLLER_ITEMS];
};

/* ---- fan cooler status (read-only) ---- */

struct NvFanCoolersStatusItem {
    uint32_t cooler_id;
    uint32_t current_rpm;
    uint32_t current_min_level;
    uint32_t current_max_level;
    uint32_t current_level;
    uint32_t _reserved[NVAPI_FAN_STATUS_ITEM_RESERVED];
};

struct NvFanCoolersStatus {
    uint32_t version;
    uint32_t count;
    uint64_t _reserved1;
    uint64_t _reserved2;
    uint64_t _reserved3;
    uint64_t _reserved4;
    NvFanCoolersStatusItem items[NVAPI_MAX_FAN_STATUS_ITEMS];
};

/*
 * Undocumented thermal sensor layout used by QueryInterface ID 0x65FE3AAD.
 * temperatures[] is fixed-point (tempC = raw / 256.0).
 */
struct NvUndocThermalSensors {
    uint32_t version;
    uint32_t mask;
    int32_t _reserved[8];
    int32_t temperatures[NVAPI_UNDOC_THERMAL_VALUES];
};

/* ---- documented thermal settings ---- */

constexpr int NVAPI_MAX_THERMAL_SETTINGS_SENSORS = 3;

enum NvThermalTarget : int32_t {
    NV_THERMAL_TARGET_NONE = 0,
    NV_THERMAL_TARGET_GPU = 1,
    NV_THERMAL_TARGET_MEMORY = 2,
    NV_THERMAL_TARGET_PS = 4,
    NV_THERMAL_TARGET_BOARD = 8,
    NV_THERMAL_TARGET_HOTSPOT = 16,
    NV_THERMAL_TARGET_ALL = 15,
    NV_THERMAL_TARGET_UNKNOWN = -1
};

enum NvThermalController : int32_t {
    NV_THERMAL_CONTROLLER_NONE = 0,
    NV_THERMAL_CONTROLLER_GPU = 1,
    NV_THERMAL_CONTROLLER_UNKNOWN = -1
};

struct NvThermalSensor {
    NvThermalController controller;
    int32_t default_min_temp;
    int32_t default_max_temp;
    int32_t current_temp;
    NvThermalTarget target;
};

struct NvGpuThermalSettings {
    uint32_t version;
    uint32_t count;
    NvThermalSensor sensor[NVAPI_MAX_THERMAL_SETTINGS_SENSORS];
};

/* ---- version helper ---- */

template <typename T>
constexpr uint32_t nvapi_make_version(int ver) {
    return static_cast<uint32_t>(sizeof(T) | (ver << 16));
}

/* ---- function pointer types (all cdecl) ---- */

typedef void* (__cdecl* NvAPI_QueryInterface_t)(uint32_t id);
typedef NvAPI_Status (__cdecl* NvAPI_Initialize_t)();
typedef NvAPI_Status (__cdecl* NvAPI_Unload_t)();
typedef NvAPI_Status (__cdecl* NvAPI_EnumPhysicalGPUs_t)(
    NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS], uint32_t* count);
typedef NvAPI_Status (__cdecl* NvAPI_GPU_GetFullName_t)(
    NvPhysicalGpuHandle handle, char name[NVAPI_SHORT_STRING_MAX]);
typedef NvAPI_Status (__cdecl* NvAPI_GPU_ClientFanCoolersGetControl_t)(
    NvPhysicalGpuHandle handle, NvFanCoolerControl* control);
typedef NvAPI_Status (__cdecl* NvAPI_GPU_ClientFanCoolersGetStatus_t)(
    NvPhysicalGpuHandle handle, NvFanCoolersStatus* status);
typedef NvAPI_Status (__cdecl* NvAPI_GPU_ClientFanCoolersSetControl_t)(
    NvPhysicalGpuHandle handle, NvFanCoolerControl* control);
typedef NvAPI_Status (__cdecl* NvAPI_GPU_GetThermalSensorsUndoc_t)(
    NvPhysicalGpuHandle handle, NvUndocThermalSensors* sensors);
typedef NvAPI_Status (__cdecl* NvAPI_GPU_GetThermalSettings_t)(
    NvPhysicalGpuHandle handle, int sensor_index, NvGpuThermalSettings* settings);
typedef NvAPI_Status (__cdecl* NvAPI_GPU_GetBusId_t)(
    NvPhysicalGpuHandle handle, uint32_t* bus_id);

#endif /* NVAPI_UNDOC_TYPES_H */
