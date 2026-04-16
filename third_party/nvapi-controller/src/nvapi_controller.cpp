#include "nvapi_controller.h"

#include "nvapi_loader.h"
#include "nvapi_fans.h"
#include "nvapi_thermals.h"
#include "nvml_loader.h"
#include "nvapi_undoc_types.h"
#include "nvapi_undoc_ids.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <shared_mutex>

/* ---- PIMPL implementation ---- */

struct NvApiControllerImpl {
    NvApiLoader   loader;
    NvApiFans     fans;
    NvApiThermals thermals;
    NvmlLoader    nvml;
    bool          nvml_available = false;
    unsigned int  nvml_device_count = 0;
    bool          initialized = false;

    /* Cached at init — never changes for the lifetime of the driver session */
    NvAPI_EnumPhysicalGPUs_t fn_enum_gpus = nullptr;
    NvAPI_GPU_GetFullName_t  fn_get_name  = nullptr;
    /* Per-GPU fan control cache.
     * Updated after every successful set_control() and on discover().
     * Used for validity/cooler-count checks; the actual write path always
     * starts from a fresh get_control() read. */
    struct CoolerSpin {
        NvFanSpinState state = NvFanSpinState::UNKNOWN;
        std::chrono::steady_clock::time_point kick_start{};
        std::chrono::steady_clock::time_point last_manual_write{};
        bool kick_active = false;
    };

    struct GpuFanCache {
        NvFanCoolerControl ctrl_template = {};
        uint32_t cooler_count = 0;
        bool valid = false;
        CoolerSpin spin[NVAPI_MAX_FAN_CONTROLLER_ITEMS] = {};
    };
    static constexpr int MAX_CACHED_GPUS = NVAPI_MAX_PHYSICAL_GPUS;
    NvmlDevice nvml_devices[MAX_CACHED_GPUS] = {};
    GpuFanCache fan_cache[MAX_CACHED_GPUS] = {};
    std::mutex  gpu_lock[MAX_CACHED_GPUS];

    /* Lifecycle lock: exclusive for init/shutdown/discover,
     * shared for everything else.  Prevents use-after-shutdown. */
    std::shared_mutex lifecycle_lock;

    NvFanSpinConfig spin_config;        /* default-constructed = disabled */
    std::mutex      spin_config_mutex;  /* protects spin_config */

    /* Incremented on every discover(); stored in each NvGpuDescriptor.
     * Write functions reject descriptors whose generation doesn't match,
     * preventing stale handle/index mispairing after re-enumeration. */
    std::atomic<uint32_t> discover_gen{0};
};

/* ---- Helpers ---- */

/* Read fresh control state, retrying once on transient failure. */
static NvAPI_Status read_fresh_control(NvApiFans& fans, NvPhysicalGpuHandle handle,
                                       NvFanCoolerControl& ctrl) {
    NvAPI_Status st = fans.get_control(handle, ctrl);
    if (st != NVAPI_OK) st = fans.get_control(handle, ctrl);
    return st;
}

static void clear_nvml_cache(NvApiControllerImpl& impl) {
    impl.nvml_available = false;
    impl.nvml_device_count = 0;
    for (auto& device : impl.nvml_devices) {
        device = nullptr;
    }
}

/* Look up live min/max for cooler_id in the status struct. */
static bool lookup_live_bounds(const NvFanCoolersStatus& status, uint32_t cooler_id,
                               uint32_t& lo, uint32_t& hi) {
    uint32_t n = std::min(status.count, static_cast<uint32_t>(NVAPI_MAX_FAN_STATUS_ITEMS));
    for (uint32_t i = 0; i < n; i++) {
        if (status.items[i].cooler_id == cooler_id) {
            lo = status.items[i].current_min_level;
            hi = status.items[i].current_max_level;
            if (hi < lo) std::swap(lo, hi);
            return true;
        }
    }
    return false;
}

/* Clamp level to effective bounds: live from get_status() if available,
 * else discover-time bounds from the descriptor, else [0..100].
 *
 * The driver's min_level (~30%) is the fan motor stall threshold — below
 * this duty cycle the motor cannot sustain rotation.  Clamping up to it
 * is intentional.  0 RPM (zero-fan idle) is only achievable via auto mode,
 * which cuts power entirely rather than setting a low duty cycle. */
static uint32_t clamp_to_bounds(uint32_t level,
                                const NvFanCoolersStatus* status,
                                uint32_t cooler_id,
                                const NvGpuDescriptor& gpu,
                                int cooler_index) {
    uint32_t lo = 0, hi = 100;
    if (status && lookup_live_bounds(*status, cooler_id, lo, hi)) {
        /* live bounds found */
    } else if (cooler_index >= 0 && cooler_index < static_cast<int>(gpu.coolers.size())) {
        lo = gpu.coolers[cooler_index].min_level;
        hi = gpu.coolers[cooler_index].max_level;
    }
    if (level < lo) level = lo;
    if (level > hi) level = hi;
    return level;
}

/* Look up current RPM for a cooler_id from status struct. Returns 0 if not found. */
static uint32_t lookup_rpm(const NvFanCoolersStatus* status, uint32_t cooler_id) {
    if (!status) return 0;
    uint32_t n = std::min(status->count, static_cast<uint32_t>(NVAPI_MAX_FAN_STATUS_ITEMS));
    for (uint32_t i = 0; i < n; i++) {
        if (status->items[i].cooler_id == cooler_id)
            return status->items[i].current_rpm;
    }
    return 0;
}

/* Apply spin state machine logic for a single cooler.
 * Fills ctrl_item mode + level and updates spin state.
 * Returns NvFanWriteResult for observability. */
static NvFanWriteResult apply_spin_logic(
    NvApiControllerImpl::GpuFanCache& cache,
    int cooler_index,
    uint32_t requested,
    const NvFanCoolersStatus* status,
    uint32_t cooler_id,
    const NvGpuDescriptor& gpu,
    const NvFanSpinConfig& cfg,
    NvFanCoolerControlItem& ctrl_item)
{
    NvFanWriteResult wr;
    wr.requested_level = requested;
    auto now = std::chrono::steady_clock::now();
    auto& spin = cache.spin[cooler_index];

    uint32_t rpm = lookup_rpm(status, cooler_id);

    /* Get live bounds for clamping when spinning */
    uint32_t run_min = 0, run_max = 100;
    bool have_live_bounds = false;
    if (status) {
        have_live_bounds = lookup_live_bounds(*status, cooler_id, run_min, run_max);
    }
    if (!have_live_bounds && cooler_index >= 0 && cooler_index < static_cast<int>(gpu.coolers.size())) {
        run_min = gpu.coolers[cooler_index].min_level;
        run_max = gpu.coolers[cooler_index].max_level;
    }

    /* Bootstrap: determine initial state from RPM */
    if (spin.state == NvFanSpinState::UNKNOWN) {
        spin.state = (rpm < cfg.rpm_stopped) ? NvFanSpinState::STOPPED : NvFanSpinState::SPINNING;
    }

    /* STALL → immediately re-kick (enter SPIN_UP) */
    if (spin.state == NvFanSpinState::STALL) {
        spin.kick_active = true;
        spin.kick_start = now;
        spin.state = NvFanSpinState::SPIN_UP;
        wr.stall_detected = true;
    }

    switch (spin.state) {
    case NvFanSpinState::STOPPED:
        if (requested >= cfg.start_threshold) {
            /* Begin kick to overcome static friction */
            if (rpm < cfg.rpm_stopped) {
                spin.kick_active = true;
                spin.kick_start = now;
                spin.state = NvFanSpinState::SPIN_UP;
                ctrl_item.control_mode = NV_FAN_MANUAL;
                ctrl_item.level = std::max(cfg.kick_level, requested);
                if (ctrl_item.level > run_max) ctrl_item.level = run_max;
                wr.effective_level = ctrl_item.level;
                wr.kick_applied = true;
                wr.state = NvFanSpinState::SPIN_UP;
                spin.last_manual_write = now;
                return wr;
            } else {
                /* RPM already up — go straight to SPINNING */
                spin.state = NvFanSpinState::SPINNING;
                /* fall through to SPINNING */
            }
        } else {
            /* requested < start_threshold: stay stopped */
            ctrl_item.control_mode = NV_FAN_AUTO;
            ctrl_item.level = 0;
            wr.effective_level = 0;
            wr.state = NvFanSpinState::STOPPED;
            return wr;
        }
        [[fallthrough]];

    case NvFanSpinState::SPIN_UP:
        if (spin.state == NvFanSpinState::SPIN_UP && spin.kick_active) {
            /* Stop request preempts kick immediately */
            if (requested <= cfg.stop_threshold) {
                spin.state = NvFanSpinState::STOPPED;
                spin.kick_active = false;
                ctrl_item.control_mode = NV_FAN_AUTO;
                ctrl_item.level = 0;
                wr.effective_level = 0;
                wr.state = NvFanSpinState::STOPPED;
                return wr;
            }
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - spin.kick_start).count();
            if (elapsed_ms < static_cast<int64_t>(cfg.kick_duration_ms)) {
                /* Still in kick window */
                ctrl_item.control_mode = NV_FAN_MANUAL;
                ctrl_item.level = std::max(cfg.kick_level, requested);
                if (ctrl_item.level > run_max) ctrl_item.level = run_max;
                wr.effective_level = ctrl_item.level;
                wr.kick_applied = true;
                wr.state = NvFanSpinState::SPIN_UP;
                spin.last_manual_write = now;
                return wr;
            }
            /* Kick expired — check if fan started */
            spin.kick_active = false;
            if (rpm >= cfg.rpm_stopped) {
                spin.state = NvFanSpinState::SPINNING;
                /* fall through to SPINNING */
            } else {
                /* Still not spinning after kick — stall */
                spin.state = NvFanSpinState::STALL;
                /* Re-kick immediately */
                spin.kick_active = true;
                spin.kick_start = now;
                spin.state = NvFanSpinState::SPIN_UP;
                ctrl_item.control_mode = NV_FAN_MANUAL;
                ctrl_item.level = std::max(cfg.kick_level, requested);
                if (ctrl_item.level > run_max) ctrl_item.level = run_max;
                wr.effective_level = ctrl_item.level;
                wr.kick_applied = true;
                wr.stall_detected = true;
                wr.state = NvFanSpinState::STALL;
                spin.last_manual_write = now;
                return wr;
            }
        }
        /* When execution arrives from SPIN_UP, state is SPINNING. Fall through. */
        [[fallthrough]];

    case NvFanSpinState::SPINNING:
        if (requested <= cfg.stop_threshold) {
            /* Stop the fan */
            spin.state = NvFanSpinState::STOPPED;
            spin.kick_active = false;
            ctrl_item.control_mode = NV_FAN_AUTO;
            ctrl_item.level = 0;
            wr.effective_level = 0;
            wr.state = NvFanSpinState::STOPPED;
            return wr;
        }
        if (requested < cfg.start_threshold) {
            /* Dead zone: below start but above stop — clamp to run_min */
            ctrl_item.control_mode = NV_FAN_MANUAL;
            ctrl_item.level = run_min;
            wr.effective_level = run_min;
        } else {
            /* Normal spinning — clamp to [run_min, run_max] */
            uint32_t clamped = requested;
            if (clamped < run_min) clamped = run_min;
            if (clamped > run_max) clamped = run_max;
            ctrl_item.control_mode = NV_FAN_MANUAL;
            ctrl_item.level = clamped;
            wr.effective_level = clamped;
        }
        /* Stall check: manual mode but RPM too low for too long.
         * Only update last_manual_write when RPM confirms spinning —
         * otherwise the stall timer would reset on every write and
         * never mature under normal control-loop write rates. */
        if (rpm < cfg.rpm_stopped) {
            auto since_write = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - spin.last_manual_write).count();
            if (since_write > static_cast<int64_t>(cfg.stall_timeout_ms)
                && spin.last_manual_write.time_since_epoch().count() != 0) {
                spin.state = NvFanSpinState::STALL;
                spin.kick_active = true;
                spin.kick_start = now;
                spin.state = NvFanSpinState::SPIN_UP;
                ctrl_item.level = std::max(cfg.kick_level, requested);
                if (ctrl_item.level > run_max) ctrl_item.level = run_max;
                wr.effective_level = ctrl_item.level;
                wr.kick_applied = true;
                wr.stall_detected = true;
                wr.state = NvFanSpinState::STALL;
                spin.last_manual_write = now;
                return wr;
            }
            /* RPM low — do NOT update last_manual_write so stall timer accumulates */
        } else {
            spin.last_manual_write = now;
        }
        wr.state = NvFanSpinState::SPINNING;
        return wr;

    default:
        break;
    }

    /* Defensive fallback path */
    ctrl_item.control_mode = NV_FAN_MANUAL;
    ctrl_item.level = clamp_to_bounds(requested, status, cooler_id, gpu, cooler_index);
    wr.effective_level = ctrl_item.level;
    wr.state = spin.state;
    return wr;
}

/* ---- NvApiController ---- */

NvApiController::NvApiController()
    : impl_(std::make_unique<NvApiControllerImpl>()) {}

NvApiController::~NvApiController() {
    shutdown();
}

NvApiController::NvApiController(NvApiController&&) noexcept = default;
NvApiController& NvApiController::operator=(NvApiController&&) noexcept = default;

bool NvApiController::init(std::string& out_warning) {
    std::unique_lock<std::shared_mutex> lifecycle(impl_->lifecycle_lock);

    if (impl_->initialized) {
        return true;
    }

    if (!impl_->loader.init(out_warning)) {
        return false;
    }

    /* Cache GPU enumeration and naming pointers once */
    impl_->fn_enum_gpus = impl_->loader.resolve<NvAPI_EnumPhysicalGPUs_t>(NVAPI_ID_ENUM_PHYSICAL_GPUS);
    impl_->fn_get_name  = impl_->loader.resolve<NvAPI_GPU_GetFullName_t>(NVAPI_ID_GPU_GET_FULL_NAME);

    if (!impl_->fn_enum_gpus) {
        out_warning = "Cannot resolve NvAPI_EnumPhysicalGPUs.";
        impl_->loader.shutdown();
        return false;
    }

    std::string fan_warn;
    if (!impl_->fans.init(impl_->loader, fan_warn)) {
        out_warning = fan_warn;
        impl_->loader.shutdown();
        return false;
    }

    std::string thermal_warn;
    if (!impl_->thermals.init(impl_->loader, thermal_warn)) {
        out_warning = thermal_warn;
        impl_->fans.shutdown();
        impl_->loader.shutdown();
        return false;
    }

    /* NVML for total board power — optional, non-fatal if unavailable */
    clear_nvml_cache(*impl_);
    std::string nvml_warn;
    if (impl_->nvml.init(nvml_warn)) {
        unsigned int count = 0;
        if (impl_->nvml.get_device_count(count) && count > 0) {
            impl_->nvml_device_count = std::min(
                count, static_cast<unsigned int>(NvApiControllerImpl::MAX_CACHED_GPUS));
            for (unsigned int i = 0; i < impl_->nvml_device_count; i++) {
                NvmlDevice device = nullptr;
                if (impl_->nvml.get_device_by_index(i, device)) {
                    impl_->nvml_devices[i] = device;
                }
            }
            impl_->nvml_available = true;
        } else {
            impl_->nvml.shutdown();
        }
    }

    impl_->initialized = true;
    return true;
}

void NvApiController::shutdown() {
    if (!impl_) return;
    std::unique_lock<std::shared_mutex> lifecycle(impl_->lifecycle_lock);
    impl_->thermals.shutdown();
    impl_->fans.shutdown();
    impl_->nvml.shutdown();
    clear_nvml_cache(*impl_);
    impl_->fn_enum_gpus = nullptr;
    impl_->fn_get_name = nullptr;
    impl_->loader.shutdown();
    impl_->initialized = false;
    for (int i = 0; i < NvApiControllerImpl::MAX_CACHED_GPUS; i++) {
        std::lock_guard<std::mutex> lk(impl_->gpu_lock[i]);
        impl_->fan_cache[i] = {};
    }
}

std::vector<NvGpuDescriptor> NvApiController::discover() {
    std::vector<NvGpuDescriptor> result;
    std::unique_lock<std::shared_mutex> lifecycle(impl_->lifecycle_lock);
    if (!impl_->initialized) {
        return result;
    }

    /* Single GPU enumeration — used by thermals and fans */
    NvPhysicalGpuHandle handles[NVAPI_MAX_PHYSICAL_GPUS] = {};
    uint32_t gpu_count = 0;
    if (impl_->fn_enum_gpus(handles, &gpu_count) != NVAPI_OK) {
        return result;
    }
    gpu_count = std::min(gpu_count, static_cast<uint32_t>(NVAPI_MAX_PHYSICAL_GPUS));

    uint32_t gen = ++impl_->discover_gen;

    /* Clear fan cache under per-GPU locks (defense-in-depth;
     * lifecycle_lock is exclusive so no concurrent readers exist). */
    for (int i = 0; i < NvApiControllerImpl::MAX_CACHED_GPUS; i++) {
        std::lock_guard<std::mutex> lk(impl_->gpu_lock[i]);
        impl_->fan_cache[i] = {};
    }

    for (uint32_t gi = 0; gi < gpu_count; gi++) {
        NvGpuDescriptor desc;
        desc.handle.opaque_ = handles[gi];
        desc.index = static_cast<int>(gi);
        desc.generation = gen;

        /* GPU name */
        char name_buf[NVAPI_SHORT_STRING_MAX] = {};
        if (impl_->fn_get_name && impl_->fn_get_name(handles[gi], name_buf) == NVAPI_OK) {
            desc.name = name_buf;
        } else {
            desc.name = "GPU " + std::to_string(gi);
        }

        /* Discover thermal sensors via per-bit v2 probe */
        NvThermalDiscovery td;
        if (impl_->thermals.discover_sensors(handles[gi], td)) {
            desc.sensor_mask = td.mask;
            desc.sensor_count = td.count;
            desc.core_index = td.core_index;
            desc.hotspot_index = td.hotspot_index;
            desc.hotspot_sensor_idx = td.documented_hotspot_sensor_idx;
            desc.mem_jnct_index = td.memj_index;
            desc.hotspot_available =
                (td.documented_hotspot_sensor_idx >= 0) || (td.hotspot_index >= 0);

            desc.sensors.reserve(td.count);
            for (int i = 0; i < td.count; i++) {
                NvSensorInfo si;
                si.index = i;
                si.initial_raw = td.initial[i];
                si.initial_c = td.initial[i] / 256.0;

                if (i == td.core_index)
                    si.role = NvSensorRole::Core;
                else if (i == td.hotspot_index)
                    si.role = NvSensorRole::Hotspot;
                else if (i == td.memj_index)
                    si.role = NvSensorRole::MemJunction;
                else
                    si.role = NvSensorRole::Unknown;

                desc.sensors.push_back(si);
            }
        }

        /* Discover coolers — pass handle directly, no re-enumeration */
        auto coolers = impl_->fans.discover_coolers(handles[gi]);
        for (int ci = 0; ci < static_cast<int>(coolers.size()); ci++) {
            NvCoolerDescriptor cd;
            cd.cooler_id = coolers[ci].cooler_id;
            cd.cooler_index = ci;
            cd.min_level = coolers[ci].min_level;
            cd.max_level = coolers[ci].max_level;
            desc.coolers.push_back(cd);
        }

        /* NVML power availability for this GPU */
        desc.nvml_power_available = impl_->nvml_available
            && gi < impl_->nvml_device_count;

        /* Cache fan control template under per-GPU lock */
        if (gi < NvApiControllerImpl::MAX_CACHED_GPUS) {
            std::lock_guard<std::mutex> lk(impl_->gpu_lock[gi]);
            NvFanCoolerControl ctrl;
            if (impl_->fans.get_control(handles[gi], ctrl) == NVAPI_OK) {
                impl_->fan_cache[gi].ctrl_template = ctrl;
                impl_->fan_cache[gi].cooler_count = std::min(
                    ctrl.count, static_cast<uint32_t>(NVAPI_MAX_FAN_CONTROLLER_ITEMS));
                impl_->fan_cache[gi].valid = true;
            }
        }

        result.push_back(std::move(desc));
    }

    return result;
}

NvApiStatus NvApiController::read_thermals(const NvGpuDescriptor& gpu, NvThermalSnapshot& snap) {
    snap = {};
    std::shared_lock<std::shared_mutex> lifecycle(impl_->lifecycle_lock);
    if (!impl_->initialized || gpu.sensor_mask == 0) {
        return NV_STATUS_ERROR;
    }

    auto handle = static_cast<NvPhysicalGpuHandle>(gpu.handle.opaque_);

    if (!impl_->thermals.read(handle, gpu.sensor_mask, snap.raw)) {
        return NV_STATUS_ERROR;
    }

    snap.valid = true;
    snap.raw_count = gpu.sensor_count;

    /* Extract core temp */
    if (gpu.core_index >= 0 && gpu.core_index < gpu.sensor_count) {
        int32_t raw = snap.raw[gpu.core_index];
        if (raw != 0 && raw != NvApiThermals::SENTINEL_RAW) {
            snap.core_temp_c = raw / 256.0;
            snap.core_valid = true;
        }
    }

    if (impl_->thermals.read_hotspot_c(handle, gpu.hotspot_sensor_idx,
                                       gpu.hotspot_index, snap.raw,
                                       gpu.sensor_count, snap.hotspot_temp_c)) {
        snap.hotspot_valid = true;
    }

    /* Extract mem junction temp */
    if (gpu.mem_jnct_index >= 0 && gpu.mem_jnct_index < gpu.sensor_count) {
        int32_t raw = snap.raw[gpu.mem_jnct_index];
        if (raw != 0 && raw != NvApiThermals::SENTINEL_RAW) {
            snap.mem_jnct_temp_c = raw / 256.0;
            snap.mem_jnct_valid = true;
        }
    }

    return NV_STATUS_OK;
}

NvApiStatus NvApiController::read_thermals_raw(const NvGpuDescriptor& gpu,
                                                std::array<int32_t, NVAPI_CONTROLLER_MAX_SENSORS>& out,
                                                int& out_count) {
    out_count = 0;
    out.fill(0);
    std::shared_lock<std::shared_mutex> lifecycle(impl_->lifecycle_lock);
    if (!impl_->initialized || gpu.sensor_mask == 0) {
        return NV_STATUS_ERROR;
    }

    auto handle = static_cast<NvPhysicalGpuHandle>(gpu.handle.opaque_);
    if (!impl_->thermals.read(handle, gpu.sensor_mask, out.data())) {
        return NV_STATUS_ERROR;
    }

    out_count = gpu.sensor_count;
    return NV_STATUS_OK;
}

NvApiStatus NvApiController::read_thermals_raw(const NvGpuDescriptor& gpu,
                                                int32_t (&out)[NVAPI_CONTROLLER_MAX_SENSORS],
                                                int& out_count) {
    out_count = 0;
    std::memset(out, 0, sizeof(int32_t) * NVAPI_CONTROLLER_MAX_SENSORS);
    std::shared_lock<std::shared_mutex> lifecycle(impl_->lifecycle_lock);
    if (!impl_->initialized || gpu.sensor_mask == 0) {
        return NV_STATUS_ERROR;
    }

    auto handle = static_cast<NvPhysicalGpuHandle>(gpu.handle.opaque_);
    if (!impl_->thermals.read(handle, gpu.sensor_mask, out)) {
        return NV_STATUS_ERROR;
    }

    out_count = gpu.sensor_count;
    return NV_STATUS_OK;
}

NvApiStatus NvApiController::set_fan_level(const NvGpuDescriptor& gpu, int cooler_index, uint32_t level) {
    return set_fan_level(gpu, cooler_index, level, nullptr);
}

NvApiStatus NvApiController::set_fan_level(const NvGpuDescriptor& gpu, int cooler_index,
                                           uint32_t level, NvFanWriteResult* result) {
    std::shared_lock<std::shared_mutex> lifecycle(impl_->lifecycle_lock);
    if (!impl_->initialized) return NV_STATUS_ERROR;
    if (gpu.index < 0 || gpu.index >= NvApiControllerImpl::MAX_CACHED_GPUS) return NV_STATUS_ERROR;
    if (gpu.generation != impl_->discover_gen.load()) return NV_STATUS_ERROR;

    std::lock_guard<std::mutex> lock(impl_->gpu_lock[gpu.index]);

    auto& cache = impl_->fan_cache[gpu.index];
    if (!cache.valid) return NV_STATUS_ERROR;

    auto handle = static_cast<NvPhysicalGpuHandle>(gpu.handle.opaque_);
    NvFanCoolerControl ctrl;
    NvAPI_Status read_st = read_fresh_control(impl_->fans, handle, ctrl);
    if (read_st != NVAPI_OK) return read_st;

    uint32_t live_count = std::min(
        ctrl.count, static_cast<uint32_t>(NVAPI_MAX_FAN_CONTROLLER_ITEMS));
    if (cooler_index < 0 || cooler_index >= static_cast<int>(live_count))
        return NV_STATUS_ERROR;

    NvFanCoolersStatus status;
    NvFanCoolersStatus* status_ptr = nullptr;
    if (impl_->fans.get_status(handle, status) == NVAPI_OK)
        status_ptr = &status;
    uint32_t cooler_id = ctrl.items[cooler_index].cooler_id;

    /* Copy spin config under its own lock */
    NvFanSpinConfig cfg;
    {
        std::lock_guard<std::mutex> scfg(impl_->spin_config_mutex);
        cfg = impl_->spin_config;
    }

    const uint32_t requested_level = level;
    NvFanWriteResult wr;
    if (cfg.enabled) {
        wr = apply_spin_logic(cache, cooler_index, requested_level, status_ptr,
                              cooler_id, gpu, cfg, ctrl.items[cooler_index]);
    } else {
        /* Legacy clamp-to-bounds path */
        uint32_t clamped = clamp_to_bounds(requested_level, status_ptr, cooler_id, gpu, cooler_index);
        ctrl.items[cooler_index].control_mode = NV_FAN_MANUAL;
        ctrl.items[cooler_index].level = clamped;
        wr.requested_level = requested_level;
        wr.effective_level = clamped;
    }

    NvAPI_Status st = impl_->fans.set_control(handle, ctrl);
    if (st == NVAPI_OK) {
        cache.ctrl_template = ctrl;
        cache.cooler_count = live_count;
    }
    if (result) *result = wr;
    return st;
}

NvApiStatus NvApiController::set_fan_level_all(const NvGpuDescriptor& gpu, uint32_t level) {
    return set_fan_level_all(gpu, level, nullptr, 0);
}

NvApiStatus NvApiController::set_fan_level_all(const NvGpuDescriptor& gpu, uint32_t level,
                                               NvFanWriteResult* results, int results_capacity) {
    std::shared_lock<std::shared_mutex> lifecycle(impl_->lifecycle_lock);
    if (!impl_->initialized) return NV_STATUS_ERROR;
    if (gpu.index < 0 || gpu.index >= NvApiControllerImpl::MAX_CACHED_GPUS) return NV_STATUS_ERROR;
    if (gpu.generation != impl_->discover_gen.load()) return NV_STATUS_ERROR;

    std::lock_guard<std::mutex> lock(impl_->gpu_lock[gpu.index]);

    auto& cache = impl_->fan_cache[gpu.index];
    if (!cache.valid) return NV_STATUS_ERROR;

    auto handle = static_cast<NvPhysicalGpuHandle>(gpu.handle.opaque_);
    NvFanCoolerControl ctrl;
    NvAPI_Status read_st = read_fresh_control(impl_->fans, handle, ctrl);
    if (read_st != NVAPI_OK) return read_st;

    uint32_t live_count = std::min(
        ctrl.count, static_cast<uint32_t>(NVAPI_MAX_FAN_CONTROLLER_ITEMS));

    NvFanCoolersStatus status;
    NvFanCoolersStatus* status_ptr = nullptr;
    if (impl_->fans.get_status(handle, status) == NVAPI_OK)
        status_ptr = &status;

    /* Copy spin config under its own lock */
    NvFanSpinConfig cfg;
    {
        std::lock_guard<std::mutex> scfg(impl_->spin_config_mutex);
        cfg = impl_->spin_config;
    }

    for (uint32_t i = 0; i < live_count; i++) {
        NvFanWriteResult wr;
        if (cfg.enabled) {
            wr = apply_spin_logic(cache, static_cast<int>(i), level, status_ptr,
                                  ctrl.items[i].cooler_id, gpu, cfg, ctrl.items[i]);
        } else {
            uint32_t clamped = clamp_to_bounds(level, status_ptr,
                                               ctrl.items[i].cooler_id,
                                               gpu, static_cast<int>(i));
            ctrl.items[i].control_mode = NV_FAN_MANUAL;
            ctrl.items[i].level = clamped;
            wr.requested_level = level;
            wr.effective_level = clamped;
        }
        if (results && static_cast<int>(i) < results_capacity)
            results[i] = wr;
    }

    NvAPI_Status st = impl_->fans.set_control(handle, ctrl);
    if (st == NVAPI_OK) {
        cache.ctrl_template = ctrl;
        cache.cooler_count = live_count;
    }
    return st;
}

NvApiStatus NvApiController::set_fan_auto(const NvGpuDescriptor& gpu) {
    std::shared_lock<std::shared_mutex> lifecycle(impl_->lifecycle_lock);
    if (!impl_->initialized) return NV_STATUS_ERROR;
    if (gpu.index < 0 || gpu.index >= NvApiControllerImpl::MAX_CACHED_GPUS) return NV_STATUS_ERROR;
    if (gpu.generation != impl_->discover_gen.load()) return NV_STATUS_ERROR;

    std::lock_guard<std::mutex> lock(impl_->gpu_lock[gpu.index]);

    auto& cache = impl_->fan_cache[gpu.index];
    if (!cache.valid) return NV_STATUS_ERROR;

    /* Read fresh state so reserved fields and count are current. */
    auto handle = static_cast<NvPhysicalGpuHandle>(gpu.handle.opaque_);
    NvFanCoolerControl ctrl;
    NvAPI_Status read_st = read_fresh_control(impl_->fans, handle, ctrl);
    if (read_st != NVAPI_OK) return read_st;

    uint32_t live_count = std::min(
        ctrl.count, static_cast<uint32_t>(NVAPI_MAX_FAN_CONTROLLER_ITEMS));

    for (uint32_t i = 0; i < live_count; i++) {
        ctrl.items[i].control_mode = NV_FAN_AUTO;
        ctrl.items[i].level = 0;
    }

    NvAPI_Status st = impl_->fans.set_control(handle, ctrl);
    if (st == NVAPI_OK) {
        cache.ctrl_template = ctrl;
        cache.cooler_count = live_count;
        /* Reset all spin states — fan is now under driver control */
        for (uint32_t i = 0; i < live_count; i++) {
            cache.spin[i].state = NvFanSpinState::STOPPED;
            cache.spin[i].kick_active = false;
        }
    }
    return st;
}

NvApiStatus NvApiController::read_fan_status(const NvGpuDescriptor& gpu, std::vector<NvFanSnapshot>& out) {
    out.clear();
    std::shared_lock<std::shared_mutex> lifecycle(impl_->lifecycle_lock);
    if (!impl_->initialized) return NV_STATUS_ERROR;

    auto handle = static_cast<NvPhysicalGpuHandle>(gpu.handle.opaque_);

    NvFanCoolersStatus status;
    NvAPI_Status st = impl_->fans.get_status(handle, status);
    if (st != NVAPI_OK) return st;

    NvFanCoolerControl control;
    bool have_control = (impl_->fans.get_control(handle, control) == NVAPI_OK);

    uint32_t n = std::min(status.count, static_cast<uint32_t>(NVAPI_MAX_FAN_STATUS_ITEMS));
    for (uint32_t i = 0; i < n; i++) {
        NvFanSnapshot snap;
        snap.cooler_id = status.items[i].cooler_id;
        snap.cooler_index = static_cast<int>(i);
        snap.current_rpm = status.items[i].current_rpm;
        snap.current_level = status.items[i].current_level;
        snap.min_level = status.items[i].current_min_level;
        snap.max_level = status.items[i].current_max_level;
        snap.is_manual = false;

        if (have_control) {
            uint32_t cn = std::min(control.count, static_cast<uint32_t>(NVAPI_MAX_FAN_CONTROLLER_ITEMS));
            for (uint32_t ci = 0; ci < cn; ci++) {
                if (control.items[ci].cooler_id == snap.cooler_id) {
                    snap.is_manual = (control.items[ci].control_mode == NV_FAN_MANUAL);
                    break;
                }
            }
        }

        out.push_back(snap);
    }

    return NV_STATUS_OK;
}

NvApiStatus NvApiController::read_power(const NvGpuDescriptor& gpu, NvPowerSnapshot& snap) {
    snap = {};
    std::shared_lock<std::shared_mutex> lifecycle(impl_->lifecycle_lock);
    if (!impl_->initialized) return NV_STATUS_ERROR;

    if (impl_->nvml_available && gpu.index >= 0
        && gpu.index < static_cast<int>(impl_->nvml_device_count)) {
        auto device = impl_->nvml_devices[gpu.index];
        unsigned int power_mw = 0;
        if (device && impl_->nvml.get_power_usage(device, power_mw)) {
            snap.nvml_available = true;
            snap.total_power_mw = power_mw;
        }
    }

    snap.valid = snap.nvml_available;
    return snap.valid ? NV_STATUS_OK : NV_STATUS_ERROR;
}

bool NvApiController::is_nvml_available() const {
    if (!impl_) return false;
    std::shared_lock<std::shared_mutex> lifecycle(impl_->lifecycle_lock);
    return impl_->nvml_available;
}

NvApiStatus NvApiController::refresh_fan_cache(const NvGpuDescriptor& gpu) {
    std::shared_lock<std::shared_mutex> lifecycle(impl_->lifecycle_lock);
    if (!impl_->initialized) return NV_STATUS_ERROR;
    if (gpu.index < 0 || gpu.index >= NvApiControllerImpl::MAX_CACHED_GPUS) return NV_STATUS_ERROR;
    if (gpu.generation != impl_->discover_gen.load()) return NV_STATUS_ERROR;

    std::lock_guard<std::mutex> lock(impl_->gpu_lock[gpu.index]);

    auto handle = static_cast<NvPhysicalGpuHandle>(gpu.handle.opaque_);
    NvFanCoolerControl ctrl;
    NvAPI_Status st = impl_->fans.get_control(handle, ctrl);
    if (st != NVAPI_OK) {
        impl_->fan_cache[gpu.index] = {};
        return st;
    }

    impl_->fan_cache[gpu.index].ctrl_template = ctrl;
    impl_->fan_cache[gpu.index].cooler_count = std::min(
        ctrl.count, static_cast<uint32_t>(NVAPI_MAX_FAN_CONTROLLER_ITEMS));
    impl_->fan_cache[gpu.index].valid = true;
    return NV_STATUS_OK;
}

void NvApiController::configure_fan_spin(const NvFanSpinConfig& config) {
    std::lock_guard<std::mutex> lk(impl_->spin_config_mutex);
    impl_->spin_config = config;
}
