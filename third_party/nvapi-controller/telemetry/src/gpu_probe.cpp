#include "gpu_telemetry/gpu_probe.h"

#include "nvapi_clocks.h"
#include "nvapi_fans_core.h"
#include "nvapi_loader.h"
#include "nvapi_thermals_core.h"
#include "nvapi_undoc_extra.h"
#include "nvapi_undoc_types.h"
#include "nvml_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

/* No library-level rate limiting.  This library is a thin driver interface —
 * the caller owns the poll cadence and decides what makes sense for their
 * hardware and workload.  We provide capability, not policy.
 *
 * Empirical reference (RTX 5090, Blackwell):
 *   sample_thermal_fast  ~5-6 ms sensor refresh (~148 Hz achieved at 5ms configured)
 *   sample_fast          not yet characterized
 *   sample_medium        not yet characterized
 *   sample_slow          not yet characterized
 *   sample_rare          not yet characterized
 * Polling faster than the sensor refresh returns duplicate readings. */

/* ================================================================== */
/* Impl                                                               */
/* ================================================================== */

struct GpuProbe::Impl {
    struct GpuBackendContext {
        GpuInfo info;
        NvPhysicalGpuHandle nvapi_handle = nullptr;
        NvmlDevice nvml_device = nullptr;
        NvThermalDiscovery thermal_disc = {};

    };

    NvApiLoader nvapi_;
    NvApiThermals thermals_;
    NvApiFans fans_;
    NvmlLoader nvml_;
    std::vector<GpuBackendContext> gpus_;

    /* NVAPI function pointers */
    NvAPI_EnumPhysicalGPUs_t fn_enum_gpus_ = nullptr;
    NvAPI_GPU_GetFullName_t fn_get_name_ = nullptr;
    NvAPI_GPU_GetAllClockFrequencies_t fn_clocks_ = nullptr;
    NvAPI_GPU_GetDynamicPStatesInfoEx_t fn_pstates_ = nullptr;
    NvAPI_GPU_GetCurrentPstate_t fn_cur_pstate_ = nullptr;
    NvAPI_GPU_GetVoltageDomainsStatus_t fn_voltage_ = nullptr;

    /* Undoc NVAPI: power topology */
    nvapi_extra::GpuClientPowerTopologyGetStatusFn fn_power_topo_ = nullptr;

    /* Documented NVAPI: PCI bus ID (for NVML device matching) */
    NvAPI_GPU_GetBusId_t fn_get_bus_id_ = nullptr;

    /* ---- Polling helpers (shared across sample tiers) ---- */
    void poll_thermals_undoc(const GpuBackendContext& g, GpuSnapshot& snap) const;
    void poll_common_medium(const GpuBackendContext& g, GpuSnapshot& snap) const;
    void poll_power(const GpuBackendContext& g, GpuSnapshot& snap) const;
    void poll_fans(const GpuBackendContext& g, GpuSnapshot& snap) const;
    void poll_slow_data(const GpuBackendContext& g, GpuSnapshot& snap) const;
};

/* ================================================================== */
/* Polling helpers                                                    */
/* ================================================================== */

void GpuProbe::Impl::poll_thermals_undoc(const GpuBackendContext& g,
                                          GpuSnapshot& snap) const {
    if (thermals_.is_ready() && g.thermal_disc.mask != 0) {
        int32_t temps[NVAPI_UNDOC_THERMAL_VALUES] = {};
        if (thermals_.read(g.nvapi_handle, g.thermal_disc.mask, temps)) {
            if (g.thermal_disc.core_index >= 0)
                snap.core_c = temps[g.thermal_disc.core_index] / 256.0;
            if (g.thermal_disc.hotspot_index >= 0) {
                double hotspot_c = temps[g.thermal_disc.hotspot_index] / 256.0;
                snap.hotspot_c = static_cast<int32_t>(std::lround(hotspot_c));
            }
            if (g.thermal_disc.memj_index >= 0)
                snap.memjn_c = temps[g.thermal_disc.memj_index] / 256.0;

            int n = std::min(g.thermal_disc.count,
                             static_cast<int>(GPU_SNAPSHOT_MAX_THERMAL_SLOTS));
            snap.thermal_slot_count = n;
            std::memcpy(snap.thermal_slots, temps, n * sizeof(int32_t));
        }
    }
}

void GpuProbe::Impl::poll_common_medium(const GpuBackendContext& g,
                                         GpuSnapshot& snap) const {
    /* NVML temp */
    if (g.info.has_nvml)
        nvml_.get_temperature(g.nvml_device, NVML_TEMPERATURE_GPU, snap.nvml_temp_c);

    /* Hotspot (shared thermal backend prefers documented sensor). */
    if (g.info.has_hotspot) {
        int32_t raw_temps[NVAPI_UNDOC_THERMAL_VALUES] = {};
        int raw_count = 0;
        if (g.thermal_disc.hotspot_index >= 0 && thermals_.is_ready()
            && g.thermal_disc.mask != 0
            && thermals_.read(g.nvapi_handle, g.thermal_disc.mask, raw_temps)) {
            raw_count = g.thermal_disc.count;
        }

        double hotspot_c = 0.0;
        if (thermals_.read_hotspot_c(
                g.nvapi_handle, g.thermal_disc.documented_hotspot_sensor_idx,
                g.thermal_disc.hotspot_index, raw_temps, raw_count, hotspot_c)) {
            snap.hotspot_c = hotspot_c;

            if (snap.thermal_slot_count == 0 && raw_count > 0) {
                int n = std::min(raw_count,
                                 static_cast<int>(GPU_SNAPSHOT_MAX_THERMAL_SLOTS));
                snap.thermal_slot_count = n;
                std::memcpy(snap.thermal_slots, raw_temps, n * sizeof(int32_t));
            }
        }
    }

    /* Clocks (NVAPI) */
    if (fn_clocks_) {
        NvAllClockFrequencies clocks;
        zero_clocks(clocks, NV_CLOCK_TYPE_CURRENT);
        if (fn_clocks_(g.nvapi_handle, &clocks) == NVAPI_OK) {
            if (clocks.domain[NV_CLOCK_DOMAIN_GRAPHICS].present)
                snap.clock_graphics_mhz = clocks.domain[NV_CLOCK_DOMAIN_GRAPHICS].freq_kHz / 1000;
            if (clocks.domain[NV_CLOCK_DOMAIN_MEMORY].present)
                snap.clock_memory_mhz = clocks.domain[NV_CLOCK_DOMAIN_MEMORY].freq_kHz / 1000;
            if (clocks.domain[NV_CLOCK_DOMAIN_VIDEO].present)
                snap.clock_video_mhz = clocks.domain[NV_CLOCK_DOMAIN_VIDEO].freq_kHz / 1000;
        }

        NvAllClockFrequencies boost;
        zero_clocks(boost, NV_CLOCK_TYPE_BOOST);
        if (fn_clocks_(g.nvapi_handle, &boost) == NVAPI_OK) {
            if (boost.domain[NV_CLOCK_DOMAIN_GRAPHICS].present)
                snap.clock_boost_mhz = boost.domain[NV_CLOCK_DOMAIN_GRAPHICS].freq_kHz / 1000;
        }
    }

    /* Clocks (NVML) */
    if (g.info.has_nvml) {
        nvml_.get_clock(g.nvml_device, NVML_CLOCK_GRAPHICS, snap.nvml_clock_graphics_mhz);
        nvml_.get_clock(g.nvml_device, NVML_CLOCK_MEM, snap.nvml_clock_memory_mhz);
        nvml_.get_clock(g.nvml_device, NVML_CLOCK_VIDEO, snap.nvml_clock_video_mhz);
    }

    /* P-state */
    if (fn_cur_pstate_) {
        int32_t ps = -1;
        if (fn_cur_pstate_(g.nvapi_handle, &ps) == NVAPI_OK)
            snap.pstate = ps;
    }

    /* Utilization (NVAPI) */
    if (fn_pstates_) {
        NvDynamicPStatesInfoEx psi;
        zero_pstates_info(psi);
        if (fn_pstates_(g.nvapi_handle, &psi) == NVAPI_OK) {
            if (psi.utilization[0].present) snap.util_gpu_pct = psi.utilization[0].percentage;
            if (psi.utilization[1].present) snap.util_fb_pct = psi.utilization[1].percentage;
            if (psi.utilization[2].present) snap.util_vid_pct = psi.utilization[2].percentage;
        }
    }

    /* Utilization (NVML) */
    if (g.info.has_nvml) {
        NvmlUtilization util{};
        if (nvml_.get_utilization(g.nvml_device, util)) {
            snap.nvml_util_gpu_pct = util.gpu;
            snap.nvml_util_mem_pct = util.memory;
        }
    }
}

void GpuProbe::Impl::poll_power(const GpuBackendContext& g,
                                 GpuSnapshot& snap) const {
    /* NVML board total */
    if (g.info.has_nvml) {
        nvml_.get_power_usage(g.nvml_device, snap.nvml_power_mw);
        if (snap.nvml_power_mw != 0)
            snap.power_source = GpuPowerSource::Nvml;
    }

    /* Undoc NVAPI per-rail topology.
     * PowerTopologyGetStatus (0xEDCF624E) returns per-domain power breakdowns,
     * NOT total board power. On RTX 5090 (count=2), domains 0 and 1 each report
     * ~83-92W regardless of actual board power (55W idle to 550W load). Values
     * do NOT track real power draw -- unreliable on this GPU generation.
     * Use nvml_power_mw for total board power. Logged here for research only. */
    if (fn_power_topo_) {
        nvapi_extra::PowerTopologyStatus topo;
        std::memset(&topo, 0, sizeof(topo));
        topo.version = nvapi_make_version<nvapi_extra::PowerTopologyStatus>(1);
        if (fn_power_topo_(g.nvapi_handle, &topo) == NVAPI_OK) {
            snap.power_rail_count = static_cast<int>(
                std::min(topo.count, static_cast<uint32_t>(GPU_SNAPSHOT_MAX_POWER_RAILS)));
            for (int i = 0; i < snap.power_rail_count; i++) {
                snap.power_rails[i].domain = topo.entries[i].domain;
                snap.power_rails[i].power_mw = topo.entries[i].power_mw;
            }
        }
    }
}

void GpuProbe::Impl::poll_fans(const GpuBackendContext& g,
                                GpuSnapshot& snap) const {
    if (fans_.is_ready()) {
        NvFanCoolersStatus status;
        if (fans_.get_status(g.nvapi_handle, status) == NVAPI_OK) {
            snap.fan_count = static_cast<int>(
                std::min(status.count, static_cast<uint32_t>(GPU_SNAPSHOT_MAX_FANS)));
            for (int i = 0; i < snap.fan_count; i++) {
                snap.fans[i].level_pct = status.items[i].current_level;
                snap.fans[i].rpm = status.items[i].current_rpm;
            }
        }
    }
}

void GpuProbe::Impl::poll_slow_data(const GpuBackendContext& g,
                                     GpuSnapshot& snap) const {
    /* Voltage */
    if (fn_voltage_) {
        NvVoltageDomainsStatus vds;
        zero_voltage(vds);
        if (fn_voltage_(g.nvapi_handle, &vds) == NVAPI_OK && vds.count > 0)
            snap.voltage_core_mv = vds.domains[0].current_mV;
    }

    /* VRAM (NVML) */
    if (g.info.has_nvml) {
        NvmlMemoryInfo mem{};
        if (nvml_.get_memory_info(g.nvml_device, mem)) {
            snap.vram_used_mb = static_cast<unsigned int>(mem.used / (1024 * 1024));
            snap.vram_free_mb = static_cast<unsigned int>(mem.free / (1024 * 1024));
            snap.vram_total_mb = static_cast<unsigned int>(mem.total / (1024 * 1024));
        }
    }

    /* Encoder/Decoder (NVML) */
    if (g.info.has_nvml) {
        unsigned int period = 0;
        nvml_.get_encoder_utilization(g.nvml_device, snap.nvml_encoder_pct, period);
        nvml_.get_decoder_utilization(g.nvml_device, snap.nvml_decoder_pct, period);
    }

    /* PCIe (NVML) */
    if (g.info.has_nvml) {
        nvml_.get_pcie_throughput(g.nvml_device, NVML_PCIE_UTIL_TX_BYTES, snap.pcie_tx_kb_s);
        nvml_.get_pcie_throughput(g.nvml_device, NVML_PCIE_UTIL_RX_BYTES, snap.pcie_rx_kb_s);
        nvml_.get_pcie_link_gen(g.nvml_device, snap.pcie_link_gen);
        nvml_.get_pcie_link_width(g.nvml_device, snap.pcie_link_width);
    }

    /* Throttle (NVML) */
    if (g.info.has_nvml)
        nvml_.get_throttle_reasons(g.nvml_device, snap.throttle_reasons);
}

/* ================================================================== */
/* Construction / lifecycle                                           */
/* ================================================================== */

GpuProbe::GpuProbe()
    : impl_(std::make_unique<Impl>()) {}

GpuProbe::~GpuProbe() = default;

GpuProbe::GpuProbe(GpuProbe&&) noexcept = default;

GpuProbe& GpuProbe::operator=(GpuProbe&&) noexcept = default;

bool GpuProbe::init(std::string& out_warning) {
    /* ---- Init NVAPI ---- */
    if (!impl_->nvapi_.init(out_warning)) return false;

    impl_->fn_enum_gpus_ =
        impl_->nvapi_.resolve<NvAPI_EnumPhysicalGPUs_t>(NVAPI_ID_ENUM_PHYSICAL_GPUS);
    impl_->fn_get_name_ =
        impl_->nvapi_.resolve<NvAPI_GPU_GetFullName_t>(NVAPI_ID_GPU_GET_FULL_NAME);
    impl_->fn_clocks_ =
        impl_->nvapi_.resolve<NvAPI_GPU_GetAllClockFrequencies_t>(NVAPI_ID_GPU_GET_ALL_CLOCK_FREQUENCIES);
    impl_->fn_pstates_ =
        impl_->nvapi_.resolve<NvAPI_GPU_GetDynamicPStatesInfoEx_t>(NVAPI_ID_GPU_GET_DYNAMIC_PSTATES_INFO_EX);
    impl_->fn_cur_pstate_ =
        impl_->nvapi_.resolve<NvAPI_GPU_GetCurrentPstate_t>(NVAPI_ID_GPU_GET_CURRENT_PSTATE);
    impl_->fn_voltage_ =
        impl_->nvapi_.resolve<NvAPI_GPU_GetVoltageDomainsStatus_t>(NVAPI_ID_GPU_GET_VOLTAGE_DOMAINS_STATUS);
    impl_->fn_power_topo_ = impl_->nvapi_.resolve<nvapi_extra::GpuClientPowerTopologyGetStatusFn>(
        nvapi_extra::kIdGpuClientPowerTopologyGetStatus);
    impl_->fn_get_bus_id_ =
        impl_->nvapi_.resolve<NvAPI_GPU_GetBusId_t>(NVAPI_ID_GPU_GET_BUS_ID);

    if (!impl_->fn_enum_gpus_) {
        out_warning = "Cannot resolve EnumPhysicalGPUs.";
        return false;
    }

    /* ---- Init thermals + fans ---- */
    std::string thermal_warning;
    impl_->thermals_.init(impl_->nvapi_, thermal_warning);
    impl_->fans_.init(impl_->nvapi_, thermal_warning);

    /* ---- Init NVML (non-fatal if missing) ---- */
    std::string nvml_warn;
    bool have_nvml = impl_->nvml_.init(nvml_warn);
    if (!have_nvml && !nvml_warn.empty()) {
        if (!out_warning.empty()) out_warning += '\n';
        out_warning += "NVML: " + nvml_warn;
    }

    /* ---- Enumerate NVAPI GPUs ---- */
    NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS] = {};
    uint32_t count = 0;
    if (impl_->fn_enum_gpus_(handles, &count) != NVAPI_OK || count == 0) {
        out_warning = "No NVIDIA GPUs found.";
        return false;
    }

    /* ---- Enumerate NVML devices for handle matching ---- */
    struct NvmlDevInfo {
        NvmlDevice dev = nullptr;
        char bus_id[32] = {};
        uint32_t pci_bus = 0;  /* parsed from bus_id for matching */
    };
    std::vector<NvmlDevInfo> nvml_devs;
    if (have_nvml) {
        unsigned int nvml_count = 0;
        if (impl_->nvml_.get_device_count(nvml_count)) {
            for (unsigned int i = 0; i < nvml_count; i++) {
                NvmlDevInfo di{};
                if (impl_->nvml_.get_device_by_index(i, di.dev)) {
                    impl_->nvml_.get_pci_bus_id(di.dev, di.bus_id, sizeof(di.bus_id));
                    /* NVML bus ID format: "00000000:BB:DD.F" -- parse BB */
                    const char* colon = std::strchr(di.bus_id, ':');
                    if (colon)
                        di.pci_bus = static_cast<uint32_t>(std::strtoul(colon + 1, nullptr, 16));
                    nvml_devs.push_back(di);
                }
            }
        }
    }

    /* ---- Build per-GPU contexts ---- */
    impl_->gpus_.clear();
    impl_->gpus_.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        auto& g = impl_->gpus_[i];
        g.info.index = static_cast<int>(i);
        g.nvapi_handle = handles[i];

        if (impl_->fn_get_name_) {
            char gpu_name[NVAPI_SHORT_STRING_MAX] = {};
            impl_->fn_get_name_(g.nvapi_handle, gpu_name);
            g.info.name = gpu_name;
        }

        /* Thermal discovery */
        impl_->thermals_.discover_sensors(g.nvapi_handle, g.thermal_disc);
        g.info.thermal.mask = g.thermal_disc.mask;
        g.info.thermal.sensor_count = g.thermal_disc.count;
        g.info.thermal.core_sensor_idx = g.thermal_disc.core_index;
        g.info.thermal.memjn_sensor_idx = g.thermal_disc.memj_index;

        g.info.has_hotspot =
            (g.thermal_disc.documented_hotspot_sensor_idx >= 0) ||
            (g.thermal_disc.hotspot_index >= 0);

        /* Match NVML device by PCI bus ID. NVAPI GetBusId returns the PCI
         * bus number; we match it against the bus number parsed from NVML's
         * full bus ID string. Falls back to index matching when GetBusId
         * is unavailable. */
        if (have_nvml && !nvml_devs.empty()) {
            bool matched = false;
            if (impl_->fn_get_bus_id_) {
                uint32_t nvapi_bus = 0;
                if (impl_->fn_get_bus_id_(g.nvapi_handle, &nvapi_bus) == NVAPI_OK) {
                    for (auto& nd : nvml_devs) {
                        if (nd.pci_bus == nvapi_bus) {
                            g.nvml_device = nd.dev;
                            g.info.has_nvml = true;
                            g.info.nvml_pci_bus_id = nd.bus_id;
                            matched = true;
                            break;
                        }
                    }
                }
            }
            if (!matched && i < nvml_devs.size()) {
                g.nvml_device = nvml_devs[i].dev;
                g.info.has_nvml = true;
                g.info.nvml_pci_bus_id = nvml_devs[i].bus_id;
            }
        }
    }

    return true;
}

void GpuProbe::shutdown() {
    impl_->gpus_.clear();
    impl_->fans_.shutdown();
    impl_->thermals_.shutdown();
    impl_->nvml_.shutdown();
    impl_->nvapi_.shutdown();
}

int GpuProbe::gpu_count() const {
    return static_cast<int>(impl_->gpus_.size());
}

const GpuInfo& GpuProbe::gpu(int index) const {
    return impl_->gpus_[index].info;
}

/* ================================================================== */
/* Sample methods                                                     */
/* ================================================================== */

bool GpuProbe::sample_fast(int gpu_index, GpuSnapshot& snap) const {
    if (gpu_index < 0 || gpu_index >= static_cast<int>(impl_->gpus_.size())) return false;
    auto& g = impl_->gpus_[gpu_index];
    snap.gpu_index = gpu_index;
    impl_->poll_thermals_undoc(g, snap);
    impl_->poll_common_medium(g, snap);
    return true;
}

bool GpuProbe::sample_slow(int gpu_index, GpuSnapshot& snap) const {
    if (gpu_index < 0 || gpu_index >= static_cast<int>(impl_->gpus_.size())) return false;
    auto& g = impl_->gpus_[gpu_index];
    impl_->poll_fans(g, snap);
    impl_->poll_power(g, snap);
    impl_->poll_slow_data(g, snap);
    return true;
}

/* ================================================================== */
/* Tiered sampling for HD mode                                        */
/* ================================================================== */

bool GpuProbe::sample_thermal_fast(int gpu_index, GpuSnapshot& snap) const {
    if (gpu_index < 0 || gpu_index >= static_cast<int>(impl_->gpus_.size())) return false;
    auto& g = impl_->gpus_[gpu_index];
    snap.gpu_index = gpu_index;
    impl_->poll_thermals_undoc(g, snap);
    return true;
}

bool GpuProbe::sample_medium(int gpu_index, GpuSnapshot& snap) const {
    if (gpu_index < 0 || gpu_index >= static_cast<int>(impl_->gpus_.size())) return false;
    auto& g = impl_->gpus_[gpu_index];
    impl_->poll_common_medium(g, snap);
    impl_->poll_power(g, snap);
    return true;
}

bool GpuProbe::sample_rare(int gpu_index, GpuSnapshot& snap) const {
    if (gpu_index < 0 || gpu_index >= static_cast<int>(impl_->gpus_.size())) return false;
    auto& g = impl_->gpus_[gpu_index];
    impl_->poll_slow_data(g, snap);
    return true;
}
