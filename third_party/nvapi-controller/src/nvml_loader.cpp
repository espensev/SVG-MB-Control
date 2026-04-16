#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "nvml_loader.h"

/* ---- NVML function signatures ---- */

using nvmlInit_v2_t = int (*)();
using nvmlShutdown_t = int (*)();
using nvmlDeviceGetCount_v2_t = int (*)(unsigned int*);
using nvmlDeviceGetHandleByIndex_v2_t = int (*)(unsigned int, void**);
using nvmlDeviceGetName_t = int (*)(void*, char*, unsigned int);
using nvmlDeviceGetPciInfo_v3_t = int (*)(void*, void*);  /* unused directly */
using nvmlDeviceGetPciBusId_t = int (*)(void*, char*, unsigned int);
using nvmlDeviceGetTemperature_t = int (*)(void*, unsigned int, unsigned int*);
using nvmlDeviceGetClockInfo_t = int (*)(void*, unsigned int, unsigned int*);
using nvmlDeviceGetPowerUsage_t = int (*)(void*, unsigned int*);
using nvmlDeviceGetUtilizationRates_t = int (*)(void*, NvmlUtilization*);
using nvmlDeviceGetPerformanceState_t = int (*)(void*, int*);
using nvmlDeviceGetMemoryInfo_t = int (*)(void*, NvmlMemoryInfo*);
using nvmlDeviceGetEncoderUtilization_t = int (*)(void*, unsigned int*, unsigned int*);
using nvmlDeviceGetDecoderUtilization_t = int (*)(void*, unsigned int*, unsigned int*);
using nvmlDeviceGetPcieThroughput_t = int (*)(void*, unsigned int, unsigned int*);
using nvmlDeviceGetCurrPcieLinkGeneration_t = int (*)(void*, unsigned int*);
using nvmlDeviceGetCurrPcieLinkWidth_t = int (*)(void*, unsigned int*);
using nvmlDeviceGetCurrentClocksThrottleReasons_t = int (*)(void*, unsigned long long*);

/* ---- Helpers ---- */

static void* load_fn(HMODULE dll, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(dll, name));
}

/* ---- Implementation ---- */

NvmlLoader::NvmlLoader() = default;

NvmlLoader::~NvmlLoader() {
    shutdown();
}

bool NvmlLoader::init(std::string& out_warning) {
    if (initialized_) return true;

    HMODULE dll = LoadLibraryW(L"nvml.dll");
    if (!dll) {
        out_warning = "Unable to load nvml.dll. Ensure NVIDIA driver is installed.";
        return false;
    }
    dll_handle_ = dll;

    fn_init_ = load_fn(dll, "nvmlInit_v2");
    fn_shutdown_ = load_fn(dll, "nvmlShutdown");
    fn_device_count_ = load_fn(dll, "nvmlDeviceGetCount_v2");
    fn_device_by_index_ = load_fn(dll, "nvmlDeviceGetHandleByIndex_v2");
    fn_device_name_ = load_fn(dll, "nvmlDeviceGetName");
    fn_pci_bus_id_ = load_fn(dll, "nvmlDeviceGetPciBusId");
    fn_temperature_ = load_fn(dll, "nvmlDeviceGetTemperature");
    fn_clock_ = load_fn(dll, "nvmlDeviceGetClockInfo");
    fn_power_ = load_fn(dll, "nvmlDeviceGetPowerUsage");
    fn_utilization_ = load_fn(dll, "nvmlDeviceGetUtilizationRates");
    fn_performance_state_ = load_fn(dll, "nvmlDeviceGetPerformanceState");
    fn_memory_info_ = load_fn(dll, "nvmlDeviceGetMemoryInfo");
    fn_encoder_util_ = load_fn(dll, "nvmlDeviceGetEncoderUtilization");
    fn_decoder_util_ = load_fn(dll, "nvmlDeviceGetDecoderUtilization");
    fn_pcie_throughput_ = load_fn(dll, "nvmlDeviceGetPcieThroughput");
    fn_pcie_link_gen_ = load_fn(dll, "nvmlDeviceGetCurrPcieLinkGeneration");
    fn_pcie_link_width_ = load_fn(dll, "nvmlDeviceGetCurrPcieLinkWidth");
    fn_throttle_reasons_ = load_fn(dll, "nvmlDeviceGetCurrentClocksEventReasons");
    if (!fn_throttle_reasons_) {
        fn_throttle_reasons_ = load_fn(dll, "nvmlDeviceGetCurrentClocksThrottleReasons");
    }

    if (!fn_init_ || !fn_shutdown_ || !fn_device_count_ || !fn_device_by_index_) {
        out_warning = "Required NVML entry points not found.";
        shutdown();
        return false;
    }

    int status = reinterpret_cast<nvmlInit_v2_t>(fn_init_)();
    if (status != 0) {
        out_warning = "nvmlInit_v2 failed (status " + std::to_string(status) + ").";
        shutdown();
        return false;
    }

    initialized_ = true;
    return true;
}

void NvmlLoader::shutdown() {
    if (initialized_ && fn_shutdown_) {
        reinterpret_cast<nvmlShutdown_t>(fn_shutdown_)();
    }

    initialized_ = false;
    fn_init_ = nullptr;
    fn_shutdown_ = nullptr;
    fn_device_count_ = nullptr;
    fn_device_by_index_ = nullptr;
    fn_device_name_ = nullptr;
    fn_pci_bus_id_ = nullptr;
    fn_temperature_ = nullptr;
    fn_clock_ = nullptr;
    fn_power_ = nullptr;
    fn_utilization_ = nullptr;
    fn_performance_state_ = nullptr;
    fn_memory_info_ = nullptr;
    fn_encoder_util_ = nullptr;
    fn_decoder_util_ = nullptr;
    fn_pcie_throughput_ = nullptr;
    fn_pcie_link_gen_ = nullptr;
    fn_pcie_link_width_ = nullptr;
    fn_throttle_reasons_ = nullptr;

    if (dll_handle_) {
        FreeLibrary(static_cast<HMODULE>(dll_handle_));
        dll_handle_ = nullptr;
    }
}

bool NvmlLoader::get_device_count(unsigned int& count) const {
    if (!fn_device_count_) return false;
    return reinterpret_cast<nvmlDeviceGetCount_v2_t>(fn_device_count_)(&count) == 0;
}

bool NvmlLoader::get_device_by_index(unsigned int index, NvmlDevice& device) const {
    if (!fn_device_by_index_) return false;
    return reinterpret_cast<nvmlDeviceGetHandleByIndex_v2_t>(fn_device_by_index_)(index, &device) == 0;
}

bool NvmlLoader::get_device_name(NvmlDevice device, char* name, unsigned int length) const {
    if (!fn_device_name_) return false;
    return reinterpret_cast<nvmlDeviceGetName_t>(fn_device_name_)(device, name, length) == 0;
}

bool NvmlLoader::get_pci_bus_id(NvmlDevice device, char* bus_id, unsigned int length) const {
    if (!fn_pci_bus_id_) return false;
    return reinterpret_cast<nvmlDeviceGetPciBusId_t>(fn_pci_bus_id_)(device, bus_id, length) == 0;
}

bool NvmlLoader::get_temperature(NvmlDevice device, NvmlTemperatureSensor sensor,
                                 unsigned int& temp_c) const {
    if (!fn_temperature_) return false;
    return reinterpret_cast<nvmlDeviceGetTemperature_t>(fn_temperature_)(
        device, static_cast<unsigned int>(sensor), &temp_c) == 0;
}

bool NvmlLoader::get_clock(NvmlDevice device, NvmlClockType type, unsigned int& clock_mhz) const {
    if (!fn_clock_) return false;
    return reinterpret_cast<nvmlDeviceGetClockInfo_t>(fn_clock_)(
        device, static_cast<unsigned int>(type), &clock_mhz) == 0;
}

bool NvmlLoader::get_power_usage(NvmlDevice device, unsigned int& power_mw) const {
    if (!fn_power_) return false;
    return reinterpret_cast<nvmlDeviceGetPowerUsage_t>(fn_power_)(device, &power_mw) == 0;
}

bool NvmlLoader::get_utilization(NvmlDevice device, NvmlUtilization& util) const {
    if (!fn_utilization_) return false;
    return reinterpret_cast<nvmlDeviceGetUtilizationRates_t>(fn_utilization_)(device, &util) == 0;
}

bool NvmlLoader::get_performance_state(NvmlDevice device, int& pstate) const {
    if (!fn_performance_state_) return false;
    return reinterpret_cast<nvmlDeviceGetPerformanceState_t>(fn_performance_state_)(device, &pstate) == 0;
}

bool NvmlLoader::get_memory_info(NvmlDevice device, NvmlMemoryInfo& info) const {
    if (!fn_memory_info_) return false;
    return reinterpret_cast<nvmlDeviceGetMemoryInfo_t>(fn_memory_info_)(device, &info) == 0;
}

bool NvmlLoader::get_encoder_utilization(NvmlDevice device, unsigned int& util_pct,
                                         unsigned int& period_us) const {
    if (!fn_encoder_util_) return false;
    return reinterpret_cast<nvmlDeviceGetEncoderUtilization_t>(fn_encoder_util_)(
        device, &util_pct, &period_us) == 0;
}

bool NvmlLoader::get_decoder_utilization(NvmlDevice device, unsigned int& util_pct,
                                         unsigned int& period_us) const {
    if (!fn_decoder_util_) return false;
    return reinterpret_cast<nvmlDeviceGetDecoderUtilization_t>(fn_decoder_util_)(
        device, &util_pct, &period_us) == 0;
}

bool NvmlLoader::get_pcie_throughput(NvmlDevice device, NvmlPcieUtilCounter counter,
                                     unsigned int& kb_per_sec) const {
    if (!fn_pcie_throughput_) return false;
    return reinterpret_cast<nvmlDeviceGetPcieThroughput_t>(fn_pcie_throughput_)(
        device, static_cast<unsigned int>(counter), &kb_per_sec) == 0;
}

bool NvmlLoader::get_pcie_link_gen(NvmlDevice device, unsigned int& gen) const {
    if (!fn_pcie_link_gen_) return false;
    return reinterpret_cast<nvmlDeviceGetCurrPcieLinkGeneration_t>(fn_pcie_link_gen_)(device, &gen) == 0;
}

bool NvmlLoader::get_pcie_link_width(NvmlDevice device, unsigned int& width) const {
    if (!fn_pcie_link_width_) return false;
    return reinterpret_cast<nvmlDeviceGetCurrPcieLinkWidth_t>(fn_pcie_link_width_)(device, &width) == 0;
}

bool NvmlLoader::get_throttle_reasons(NvmlDevice device, uint64_t& reasons) const {
    if (!fn_throttle_reasons_) return false;
    return reinterpret_cast<nvmlDeviceGetCurrentClocksThrottleReasons_t>(fn_throttle_reasons_)(
        device, &reasons) == 0;
}
