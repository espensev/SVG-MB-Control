#ifndef NVAPI_CLOCKS_H
#define NVAPI_CLOCKS_H

#include "nvapi_undoc_types.h"

#include <cstdint>
#include <cstring>

/*
 * Documented NVAPI bindings for clock frequencies, P-states, utilization,
 * and voltage domains.  All resolved via nvapi_QueryInterface.
 */

/* ---- QueryInterface IDs ---- */

constexpr uint32_t NVAPI_ID_GPU_GET_ALL_CLOCK_FREQUENCIES   = 0xDCB616C3;
constexpr uint32_t NVAPI_ID_GPU_GET_DYNAMIC_PSTATES_INFO_EX = 0x60DED2ED;
constexpr uint32_t NVAPI_ID_GPU_GET_CURRENT_PSTATE          = 0x927DA4F6;
constexpr uint32_t NVAPI_ID_GPU_GET_VOLTAGE_DOMAINS_STATUS  = 0xC16C7E2C;

/* ---- Constants ---- */

constexpr int NVAPI_MAX_GPU_CLOCKS          = 32;
constexpr int NVAPI_MAX_GPU_PSTATES         = 16;
constexpr int NVAPI_MAX_GPU_UTILIZATIONS    = 8;
constexpr int NVAPI_MAX_VOLTAGE_DOMAINS     = 16;

/* ---- Clock type enum ---- */

enum NvClockType : uint32_t {
    NV_CLOCK_TYPE_CURRENT = 0,
    NV_CLOCK_TYPE_BASE    = 1,
    NV_CLOCK_TYPE_BOOST   = 2,
};

/* ---- NvAPI_GPU_GetAllClockFrequencies ---- */

struct NvClockDomainEntry {
    uint32_t present;       /* 1 if this domain is active */
    uint32_t freq_kHz;      /* frequency in kHz */
};

struct NvAllClockFrequencies {
    uint32_t version;
    uint32_t clock_type;    /* NvClockType */
    NvClockDomainEntry domain[NVAPI_MAX_GPU_CLOCKS];
};

/* Known clock domain indices */
constexpr int NV_CLOCK_DOMAIN_GRAPHICS = 0;
constexpr int NV_CLOCK_DOMAIN_MEMORY   = 4;
constexpr int NV_CLOCK_DOMAIN_VIDEO    = 8;

/* ---- NvAPI_GPU_GetDynamicPStatesInfoEx ---- */

struct NvDynamicPStatesUtilEntry {
    uint32_t present;       /* 1 if active */
    int32_t  percentage;    /* 0-100 */
};

struct NvDynamicPStatesInfoEx {
    uint32_t version;
    uint32_t flags;         /* bit 0: GPU idle flag */
    NvDynamicPStatesUtilEntry utilization[NVAPI_MAX_GPU_UTILIZATIONS];
    /* [0]=GPU, [1]=FB(mem), [2]=VID, [3]=BUS */
};

/* ---- NvAPI_GPU_GetCurrentPstate ---- */

/* P-state values: 0=P0, 1=P1, ..., 15=P15, -1=unknown */

/* ---- NvAPI_GPU_GetVoltageDomainsStatus ---- */

struct NvVoltageDomainEntry {
    uint32_t domain_id;
    uint32_t current_mV;    /* millivolts */
};

struct NvVoltageDomainsStatus {
    uint32_t version;
    uint32_t count;
    NvVoltageDomainEntry domains[NVAPI_MAX_VOLTAGE_DOMAINS];
};

/* ---- Function pointer types ---- */

typedef NvAPI_Status (__cdecl* NvAPI_GPU_GetAllClockFrequencies_t)(
    NvPhysicalGpuHandle handle, NvAllClockFrequencies* clocks);
typedef NvAPI_Status (__cdecl* NvAPI_GPU_GetDynamicPStatesInfoEx_t)(
    NvPhysicalGpuHandle handle, NvDynamicPStatesInfoEx* info);
typedef NvAPI_Status (__cdecl* NvAPI_GPU_GetCurrentPstate_t)(
    NvPhysicalGpuHandle handle, int32_t* pstate);
typedef NvAPI_Status (__cdecl* NvAPI_GPU_GetVoltageDomainsStatus_t)(
    NvPhysicalGpuHandle handle, NvVoltageDomainsStatus* status);

/* ---- Helpers ---- */

inline void zero_clocks(NvAllClockFrequencies& c, NvClockType type = NV_CLOCK_TYPE_CURRENT) {
    std::memset(&c, 0, sizeof(c));
    c.version = nvapi_make_version<NvAllClockFrequencies>(2);
    c.clock_type = type;
}

inline void zero_pstates_info(NvDynamicPStatesInfoEx& p) {
    std::memset(&p, 0, sizeof(p));
    p.version = nvapi_make_version<NvDynamicPStatesInfoEx>(1);
}

inline void zero_voltage(NvVoltageDomainsStatus& v) {
    std::memset(&v, 0, sizeof(v));
    v.version = nvapi_make_version<NvVoltageDomainsStatus>(1);
}

#endif /* NVAPI_CLOCKS_H */
