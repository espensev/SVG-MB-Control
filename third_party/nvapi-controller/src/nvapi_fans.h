#ifndef NVAPI_FANS_H
#define NVAPI_FANS_H

#include "nvapi_loader.h"

#include <cstdint>
#include <string>
#include <vector>

/* Minimal cooler discovery result (internal). */
struct NvCoolerDiscovery {
    uint32_t cooler_id  = 0;
    uint32_t min_level  = 0;
    uint32_t max_level  = 100;
};

class NvApiFans {
public:
    NvApiFans() = default;

    bool init(const NvApiLoader& loader, std::string& out_warning);
    void shutdown();

    bool is_ready() const { return ready_; }

    /* Discover coolers for a specific GPU handle. */
    std::vector<NvCoolerDiscovery> discover_coolers(NvPhysicalGpuHandle gpu) const;

    NvAPI_Status get_control(NvPhysicalGpuHandle gpu, NvFanCoolerControl& out) const;
    NvAPI_Status get_status(NvPhysicalGpuHandle gpu, NvFanCoolersStatus& out) const;
    NvAPI_Status set_control(NvPhysicalGpuHandle gpu, NvFanCoolerControl& ctrl) const;

private:
    bool ready_ = false;
    NvAPI_GPU_ClientFanCoolersGetControl_t fn_get_control_ = nullptr;
    NvAPI_GPU_ClientFanCoolersGetStatus_t fn_get_status_ = nullptr;
    NvAPI_GPU_ClientFanCoolersSetControl_t fn_set_control_ = nullptr;
};

#endif /* NVAPI_FANS_H */
