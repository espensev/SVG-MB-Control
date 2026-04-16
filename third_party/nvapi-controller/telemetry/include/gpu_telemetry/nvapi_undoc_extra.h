#ifndef NVAPI_UNDOC_EXTRA_H
#define NVAPI_UNDOC_EXTRA_H

#include "nvapi_undoc_types.h"

#include <cstdint>

/*
 * Local compatibility surface for legacy undocumented calls that are used by
 * the test harness but are no longer part of the synced core headers.
 *
 * Keep this file test-repo local. Synced headers from nvapi_controller may
 * evolve independently, while harness code should remain stable.
 */
namespace nvapi_extra {

/* ---- legacy function IDs (for nvapi_QueryInterface) ---- */

constexpr uint32_t kIdGpuGetTachReading = 0x5F608315;
constexpr uint32_t kIdGpuGetThermalSettings = 0xE3640A56;
constexpr uint32_t kIdGpuClientPowerTopologyGetStatus = 0xEDCF624E;
constexpr uint32_t kIdGpuClientPowerPoliciesGetInfo = 0x34206D86;
constexpr uint32_t kIdGpuClientPowerPoliciesGetStatus = 0x70916171;

/* ---- legacy constants ---- */

constexpr int kMaxThermalSensors = 3;
constexpr int kMaxPowerTopoEntries = 4;
constexpr int kMaxPowerPolicyEntries = 4;

/* ---- documented thermal settings ---- */

enum ThermalTarget : int32_t {
    ThermalTargetNone = 0,
    ThermalTargetGpu = 1,
    ThermalTargetMemory = 2,
    ThermalTargetPs = 4,
    ThermalTargetBoard = 8,
    ThermalTargetHotspot = 16,
    ThermalTargetAll = 15,
    ThermalTargetUnknown = -1
};

enum ThermalController : int32_t {
    ThermalControllerNone = 0,
    ThermalControllerGpu = 1,
    ThermalControllerUnknown = -1
};

struct Sensor {
    ThermalController controller;
    int32_t default_min_temp;
    int32_t default_max_temp;
    int32_t current_temp;
    ThermalTarget target;
};

struct GpuThermalSettings {
    uint32_t version;
    uint32_t count;
    Sensor sensor[kMaxThermalSensors];
};

/* ---- undocumented power topology ---- */

struct PowerTopologyEntry {
    uint32_t domain;
    uint32_t _reserved;
    uint32_t power_mw;
    uint32_t _reserved2;
};

struct PowerTopologyStatus {
    uint32_t version;
    uint32_t count;
    PowerTopologyEntry entries[kMaxPowerTopoEntries];
};

/* ---- undocumented power policy limits ---- */

struct PowerPoliciesInfoEntry {
    uint32_t perf_state_id;
    uint32_t _reserved1[2];
    uint32_t min_power;
    uint32_t _reserved2[2];
    uint32_t default_power;
    uint32_t _reserved3[2];
    uint32_t max_power;
    uint32_t _reserved4;
};

struct PowerPoliciesInfo {
    uint32_t version;
    uint8_t flags;
    uint8_t count;
    uint8_t _pad[2];
    PowerPoliciesInfoEntry entries[kMaxPowerPolicyEntries];
};

struct PowerPoliciesStatusEntry {
    uint32_t perf_state_id;
    uint32_t _reserved1;
    uint32_t power;
    uint32_t _reserved2;
};

struct PowerPoliciesStatus {
    uint32_t version;
    uint32_t count;
    PowerPoliciesStatusEntry entries[kMaxPowerPolicyEntries];
};

/* ---- function pointer types (all cdecl) ---- */

typedef NvAPI_Status (__cdecl* GpuGetTachReadingFn)(
    NvPhysicalGpuHandle handle, int32_t* rpm);
typedef NvAPI_Status (__cdecl* GpuGetThermalSettingsFn)(
    NvPhysicalGpuHandle handle, int sensor_index, GpuThermalSettings* settings);
typedef NvAPI_Status (__cdecl* GpuClientPowerTopologyGetStatusFn)(
    NvPhysicalGpuHandle handle, PowerTopologyStatus* status);
typedef NvAPI_Status (__cdecl* GpuClientPowerPoliciesGetInfoFn)(
    NvPhysicalGpuHandle handle, PowerPoliciesInfo* info);
typedef NvAPI_Status (__cdecl* GpuClientPowerPoliciesGetStatusFn)(
    NvPhysicalGpuHandle handle, PowerPoliciesStatus* status);

} /* namespace nvapi_extra */

#endif /* NVAPI_UNDOC_EXTRA_H */
