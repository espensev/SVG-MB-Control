#ifndef GPU_TELEMETRY_NVML_LOADER_H
#define GPU_TELEMETRY_NVML_LOADER_H

#include <cstdint>
#include <string>

/*
 * Dynamic NVML wrapper. Loads nvml.dll at runtime via GetProcAddress.
 * Each method returns bool for graceful failure on unsupported GPUs.
 */

/* Opaque NVML device handle (void* to avoid NVML header dependency) */
typedef void* NvmlDevice;

/*
 * Throttle reason bitmasks from nvml.h.
 *
 * NVML defines two overlapping enum spaces that share the same bitmask
 * field (nvmlClocksThrottleReasons vs nvmlClocksEventReasons). The
 * "SW_*" / "HW_*" names below are the nvmlClocksEventReasons aliases;
 * several intentionally share values with the older throttle-reason
 * constants (e.g. SW_POWER_CAP == POWER_LIMIT == 0x04).
 */
namespace nvml_throttle {
    constexpr uint64_t NONE                = 0x0000000000000000ULL;
    constexpr uint64_t GPU_IDLE            = 0x0000000000000001ULL;
    constexpr uint64_t APPLICATIONS_CLOCKS = 0x0000000000000002ULL;
    constexpr uint64_t SW_POWER_CAP        = 0x0000000000000004ULL;
    constexpr uint64_t HW_SLOWDOWN         = 0x0000000000000008ULL;
    constexpr uint64_t SYNC_BOOST          = 0x0000000000000010ULL;
    constexpr uint64_t SW_THERMAL_SLOWDOWN = 0x0000000000000020ULL;
    constexpr uint64_t HW_THERMAL          = 0x0000000000000040ULL;
    constexpr uint64_t HW_POWER_BRAKE      = 0x0000000000000080ULL;
    constexpr uint64_t DISPLAY_CLOCKS      = 0x0000000000000100ULL;
}

/* NVML utilization struct */
struct NvmlUtilization {
    unsigned int gpu;       /* percent 0-100 */
    unsigned int memory;    /* percent 0-100 */
};

/* NVML memory info */
struct NvmlMemoryInfo {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

/* NVML PCIe throughput counter type */
enum NvmlPcieUtilCounter : unsigned int {
    NVML_PCIE_UTIL_TX_BYTES = 0,
    NVML_PCIE_UTIL_RX_BYTES = 1,
};

/* NVML temperature sensor */
enum NvmlTemperatureSensor : unsigned int {
    NVML_TEMPERATURE_GPU = 0,
};

/* NVML clock type */
enum NvmlClockType : unsigned int {
    NVML_CLOCK_GRAPHICS = 0,
    NVML_CLOCK_SM       = 1,
    NVML_CLOCK_MEM      = 2,
    NVML_CLOCK_VIDEO    = 3,
};

class NvmlLoader {
public:
    NvmlLoader();
    ~NvmlLoader();

    NvmlLoader(const NvmlLoader&) = delete;
    NvmlLoader& operator=(const NvmlLoader&) = delete;

    bool init(std::string& out_warning);
    void shutdown();

    bool is_ready() const { return initialized_; }

    /* Device enumeration */
    bool get_device_count(unsigned int& count) const;
    bool get_device_by_index(unsigned int index, NvmlDevice& device) const;
    bool get_device_name(NvmlDevice device, char* name, unsigned int length) const;
    bool get_pci_bus_id(NvmlDevice device, char* bus_id, unsigned int length) const;

    /* Thermals */
    bool get_temperature(NvmlDevice device, NvmlTemperatureSensor sensor, unsigned int& temp_c) const;

    /* Clocks */
    bool get_clock(NvmlDevice device, NvmlClockType type, unsigned int& clock_mhz) const;

    /* Power */
    bool get_power_usage(NvmlDevice device, unsigned int& power_mw) const;

    /* Utilization */
    bool get_utilization(NvmlDevice device, NvmlUtilization& util) const;

    /* Performance state */
    bool get_performance_state(NvmlDevice device, int& pstate) const;

    /* Memory */
    bool get_memory_info(NvmlDevice device, NvmlMemoryInfo& info) const;

    /* Encoder/Decoder utilization */
    bool get_encoder_utilization(NvmlDevice device, unsigned int& util_pct, unsigned int& period_us) const;
    bool get_decoder_utilization(NvmlDevice device, unsigned int& util_pct, unsigned int& period_us) const;

    /* PCIe */
    bool get_pcie_throughput(NvmlDevice device, NvmlPcieUtilCounter counter, unsigned int& kb_per_sec) const;
    bool get_pcie_link_gen(NvmlDevice device, unsigned int& gen) const;
    bool get_pcie_link_width(NvmlDevice device, unsigned int& width) const;

    /* Throttle */
    bool get_throttle_reasons(NvmlDevice device, uint64_t& reasons) const;

private:
    void* dll_handle_ = nullptr;
    bool initialized_ = false;

    /* Function pointers (all stored as void*, cast at call site) */
    void* fn_init_              = nullptr;
    void* fn_shutdown_          = nullptr;
    void* fn_device_count_      = nullptr;
    void* fn_device_by_index_   = nullptr;
    void* fn_device_name_       = nullptr;
    void* fn_pci_bus_id_        = nullptr;
    void* fn_temperature_       = nullptr;
    void* fn_clock_             = nullptr;
    void* fn_power_             = nullptr;
    void* fn_utilization_       = nullptr;
    void* fn_performance_state_ = nullptr;
    void* fn_memory_info_       = nullptr;
    void* fn_encoder_util_      = nullptr;
    void* fn_decoder_util_      = nullptr;
    void* fn_pcie_throughput_   = nullptr;
    void* fn_pcie_link_gen_     = nullptr;
    void* fn_pcie_link_width_   = nullptr;
    void* fn_throttle_reasons_  = nullptr;
};

#endif /* GPU_TELEMETRY_NVML_LOADER_H */
