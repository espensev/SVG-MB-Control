#include "nvapi_fans.h"

#include <algorithm>
#include <cstring>

namespace {

void zero_control(NvFanCoolerControl& c) noexcept {
    std::memset(&c, 0, sizeof(c));
    c.version = nvapi_make_version<NvFanCoolerControl>(1);
}

void zero_status(NvFanCoolersStatus& s) noexcept {
    std::memset(&s, 0, sizeof(s));
    s.version = nvapi_make_version<NvFanCoolersStatus>(1);
}

} // namespace

bool NvApiFans::init(const NvApiLoader& loader, std::string& out_warning) {
    if (!loader.is_ready()) {
        out_warning = "NVAPI loader is not ready.";
        return false;
    }

    fn_get_control_ = loader.resolve<NvAPI_GPU_ClientFanCoolersGetControl_t>(
        NVAPI_ID_GPU_CLIENT_FAN_COOLERS_GET_CONTROL);
    fn_get_status_ = loader.resolve<NvAPI_GPU_ClientFanCoolersGetStatus_t>(
        NVAPI_ID_GPU_CLIENT_FAN_COOLERS_GET_STATUS);
    fn_set_control_ = loader.resolve<NvAPI_GPU_ClientFanCoolersSetControl_t>(
        NVAPI_ID_GPU_CLIENT_FAN_COOLERS_SET_CONTROL);

    if (!fn_get_control_ || !fn_get_status_ || !fn_set_control_) {
        out_warning = "Required NVAPI fan control function pointers are not available.";
        shutdown();
        return false;
    }

    ready_ = true;
    return true;
}

void NvApiFans::shutdown() {
    ready_ = false;
    fn_get_control_ = nullptr;
    fn_get_status_ = nullptr;
    fn_set_control_ = nullptr;
}

std::vector<NvCoolerDiscovery> NvApiFans::discover_coolers(NvPhysicalGpuHandle gpu) const {
    std::vector<NvCoolerDiscovery> result;
    if (!ready_) return result;

    NvFanCoolerControl control;
    NvFanCoolersStatus status;
    zero_control(control);
    zero_status(status);

    if (fn_get_control_(gpu, &control) != NVAPI_OK) return result;
    if (fn_get_status_(gpu, &status) != NVAPI_OK) return result;

    uint32_t cooler_count = std::min(
        control.count, static_cast<uint32_t>(NVAPI_MAX_FAN_CONTROLLER_ITEMS));

    for (uint32_t ci = 0; ci < cooler_count; ci++) {
        uint32_t cooler_id = control.items[ci].cooler_id;

        uint32_t min_level = 0, max_level = 100;
        uint32_t status_count = std::min(
            status.count, static_cast<uint32_t>(NVAPI_MAX_FAN_STATUS_ITEMS));

        for (uint32_t si = 0; si < status_count; si++) {
            if (status.items[si].cooler_id == cooler_id) {
                min_level = status.items[si].current_min_level;
                max_level = status.items[si].current_max_level;
                break;
            }
        }

        NvCoolerDiscovery info;
        info.cooler_id = cooler_id;
        info.min_level = min_level;
        info.max_level = max_level;
        result.push_back(info);
    }

    return result;
}

NvAPI_Status NvApiFans::get_control(NvPhysicalGpuHandle gpu, NvFanCoolerControl& out) const {
    if (!ready_) return -1;
    zero_control(out);
    return fn_get_control_(gpu, &out);
}

NvAPI_Status NvApiFans::get_status(NvPhysicalGpuHandle gpu, NvFanCoolersStatus& out) const {
    if (!ready_) return -1;
    zero_status(out);
    return fn_get_status_(gpu, &out);
}

NvAPI_Status NvApiFans::set_control(NvPhysicalGpuHandle gpu, NvFanCoolerControl& ctrl) const {
    if (!ready_) return -1;
    return fn_set_control_(gpu, &ctrl);
}
