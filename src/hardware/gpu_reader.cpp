#include "gpu_reader.h"

#ifdef SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED
#include "gpu_telemetry/gpu_sensor_reader.h"
#endif

#include "env_util.h"
#include "numeric_parse.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <utility>

namespace svg_mb_control {

namespace {

constexpr std::chrono::milliseconds kGpuContextRefreshInterval{1000};

struct CachedGpuContext {
    bool valid = false;
    std::uint64_t sample_id = 0u;
    double time_ms = std::numeric_limits<double>::quiet_NaN();
    std::chrono::steady_clock::time_point sampled_at{};
    std::int32_t util_gpu_pct = -1;
    std::int32_t util_mem_pct = -1;
    std::int32_t pstate = -1;
    std::uint32_t clock_graphics_mhz = 0u;
    std::uint32_t clock_memory_mhz = 0u;
    std::uint32_t vram_used_mb = 0u;
    std::uint32_t vram_total_mb = 0u;
};

std::string SimGpuMode() {
    return GetEnvOrDefault("SVG_MB_CONTROL_SIM_GPU_MODE", "disabled");
}

bool SimGpuEnabled() {
    const std::string mode = SimGpuMode();
    return mode == "enabled" || mode == "1" || mode == "true" ||
           mode == "TRUE" || mode == "True";
}

bool SimGpuUnavailable() {
    const std::string mode = SimGpuMode();
    return mode == "unavailable" || mode == "none" || mode == "off";
}

std::string NormalizeGpuSampleMode(std::string_view mode_label) {
    std::string normalized;
    normalized.reserve(mode_label.size());
    for (const char ch : mode_label) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (ch == '_') {
            normalized.push_back('-');
        } else {
            normalized.push_back(
                static_cast<char>(std::tolower(uch)));
        }
    }
    return normalized;
}

std::string ResolveGpuEvidenceSampleModeLabel(
    std::string_view mode_label,
    std::string* warning) {
    if (warning != nullptr) {
        warning->clear();
    }
    const std::string normalized = NormalizeGpuSampleMode(mode_label);
    if (normalized.empty() || normalized == "full") {
        return "full";
    }
    if (normalized == "thermal-fast" || normalized == "thermalfast") {
        return "thermal-fast";
    }
    if (normalized == "fast" || normalized == "medium" ||
        normalized == "slow" || normalized == "rare") {
        return normalized;
    }
    if (warning != nullptr) {
        *warning = "invalid GPU evidence sample mode '";
        *warning += std::string(mode_label);
        *warning += "'; using full";
    }
    return "full";
}

std::string EffectiveRequestedModeLabel(std::string_view mode_label) {
    return mode_label.empty() ? std::string("full") : std::string(mode_label);
}

double GetDoubleEnvOrDefault(const char* name, double fallback) {
    const std::string value = GetEnvOrDefault(name, "");
    if (value.empty()) {
        return fallback;
    }
    double parsed = 0.0;
    return TryParseDoubleStrict(value, parsed) ? parsed : fallback;
}

std::uint32_t GetUint32EnvOrDefault(const char* name,
                                    std::uint32_t fallback) {
    const std::string value = GetEnvOrDefault(name, "");
    if (value.empty()) {
        return fallback;
    }
    std::uint32_t parsed = 0u;
    return TryParseIntegralStrict(value, parsed) ? parsed : fallback;
}

std::int32_t GetInt32EnvOrDefault(const char* name, std::int32_t fallback) {
    const std::string value = GetEnvOrDefault(name, "");
    if (value.empty()) {
        return fallback;
    }
    std::int32_t parsed = 0;
    return TryParseIntegralStrict(value, parsed) ? parsed : fallback;
}

std::uint64_t GetUint64EnvOrDefault(const char* name,
                                    std::uint64_t fallback) {
    const std::string value = GetEnvOrDefault(name, "");
    if (value.empty()) {
        return fallback;
    }
    std::uint64_t parsed = 0u;
    return TryParseIntegralStrict(value, parsed) ? parsed : fallback;
}

std::int64_t GetInt64EnvOrDefault(const char* name, std::int64_t fallback) {
    const std::string value = GetEnvOrDefault(name, "");
    if (value.empty()) {
        return fallback;
    }
    std::int64_t parsed = 0;
    return TryParseIntegralStrict(value, parsed) ? parsed : fallback;
}

// Monotonic milliseconds since the first call, used to stamp the GPU power read
// time (FEAT-0020 read-timestamp). The origin is process-wide so consecutive
// rows are directly comparable; the absolute value is not wall-clock.
std::int64_t MonotonicGpuReadMs() {
    static const auto origin = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - origin)
        .count();
}

// FEAT-0020: finalize the GPU power read identity. A fresh nonzero read
// (acquisition == "nvml") advances the per-reader counter and stamps the read
// timestamp; any other outcome leaves the counter where it is (a repeated
// sample id makes a stale/missing value explicit) and blanks the timestamp.
void FinalizeGpuPowerIdentity(GpuTempSample& out, std::uint64_t& counter) {
    if (out.power_acquisition == "nvml") {
        out.power_sample_id = ++counter;
        out.power_time_ms = static_cast<double>(MonotonicGpuReadMs());
    } else {
        out.power_sample_id = counter;
        out.power_time_ms = std::numeric_limits<double>::quiet_NaN();
    }
}

void ResetGpuContextFields(GpuTempSample& out, std::string acquisition) {
    out.context_acquisition = std::move(acquisition);
    out.context_sample_id = 0u;
    out.context_time_ms = std::numeric_limits<double>::quiet_NaN();
    out.context_sample_age_ms = std::numeric_limits<double>::quiet_NaN();
    out.context_util_gpu_pct = -1;
    out.context_util_mem_pct = -1;
    out.context_pstate = -1;
    out.context_clock_graphics_mhz = 0u;
    out.context_clock_memory_mhz = 0u;
    out.context_vram_used_mb = 0u;
    out.context_vram_total_mb = 0u;
}

bool CachedGpuContextHasValue(const CachedGpuContext& cache) {
    return cache.util_gpu_pct >= 0 ||
           cache.util_mem_pct >= 0 ||
           cache.pstate >= 0 ||
           cache.clock_graphics_mhz != 0u ||
           cache.clock_memory_mhz != 0u ||
           cache.vram_total_mb != 0u;
}

void ApplyCachedGpuContext(GpuTempSample& out,
                           const CachedGpuContext& cache,
                           std::chrono::steady_clock::time_point now,
                           std::string_view fallback_acquisition) {
    if (!cache.valid) {
        ResetGpuContextFields(out, std::string(fallback_acquisition));
        return;
    }
    out.context_acquisition = "nvml";
    out.context_sample_id = cache.sample_id;
    out.context_time_ms = cache.time_ms;
    out.context_sample_age_ms =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - cache.sampled_at).count());
    out.context_util_gpu_pct = cache.util_gpu_pct;
    out.context_util_mem_pct = cache.util_mem_pct;
    out.context_pstate = cache.pstate;
    out.context_clock_graphics_mhz = cache.clock_graphics_mhz;
    out.context_clock_memory_mhz = cache.clock_memory_mhz;
    out.context_vram_used_mb = cache.vram_used_mb;
    out.context_vram_total_mb = cache.vram_total_mb;
}

void FinalizeGpuContextIdentity(GpuTempSample& out,
                                std::uint64_t& counter) {
    if (out.context_acquisition == "nvml") {
        out.context_sample_id = ++counter;
        out.context_time_ms = static_cast<double>(MonotonicGpuReadMs());
        out.context_sample_age_ms = 0.0;
    } else {
        out.context_sample_id = 0u;
        out.context_time_ms = std::numeric_limits<double>::quiet_NaN();
        out.context_sample_age_ms = std::numeric_limits<double>::quiet_NaN();
    }
}

void FillSimGpuContext(GpuTempSample& out) {
    out.context_acquisition = GetEnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_CONTEXT_ACQUISITION", "nvml");
    if (out.context_acquisition != "nvml") {
        ResetGpuContextFields(out, out.context_acquisition);
        return;
    }
    out.context_util_gpu_pct = GetInt32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_UTIL_GPU_PCT", 67);
    out.context_util_mem_pct = GetInt32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_UTIL_MEM_PCT", 21);
    out.context_pstate =
        GetInt32EnvOrDefault("SVG_MB_CONTROL_SIM_GPU_PSTATE", 0);
    out.context_clock_graphics_mhz = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_CLOCK_GRAPHICS_MHZ", 2500u);
    out.context_clock_memory_mhz = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_CLOCK_MEMORY_MHZ", 10500u);
    out.context_vram_used_mb = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_VRAM_USED_MB", 8192u);
    out.context_vram_total_mb = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_VRAM_TOTAL_MB", 16384u);
}

GpuTempSample MakeSimGpuTempSample() {
    GpuTempSample out;
    out.available = true;
    out.core_c = GetDoubleEnvOrDefault("SVG_MB_CONTROL_SIM_GPU_CORE_C", 62.5);
    out.memjn_c = GetDoubleEnvOrDefault("SVG_MB_CONTROL_SIM_GPU_MEMJN_C", 72.25);
    out.hotspot_c = GetDoubleEnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_HOTSPOT_C", 80.75);
    out.gpu_name = GetEnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_NAME", "Simulated NVIDIA GPU");
    // FEAT-0020 simulated GPU board power. Default 0 means "no power reading"
    // so GPU-thermal sim tests that do not opt in keep blank power (no false
    // zero); a nonzero value simulates a fresh "nvml" board-power read.
    const std::uint32_t sim_power_mw = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_NVML_POWER_MW", 0u);
    if (sim_power_mw != 0u) {
        out.power_mw = static_cast<double>(sim_power_mw);
        out.power_source = GetEnvOrDefault(
            "SVG_MB_CONTROL_SIM_GPU_POWER_SOURCE", "nvml");
        out.power_acquisition = "nvml";
    } else {
        out.power_acquisition = "unavailable";
    }
    FillSimGpuContext(out);
    return out;
}

// Category fillers for the simulated GPU evidence path. Each mutates the
// fields it owns and reads its env-var defaults; together they collapse the
// ~100 sequential env-var assignments in MakeSimGpuEvidenceSample into one
// linear list of named groups so it is obvious which env vars feed which
// field block.

void FillSimGpuTemperatures(GpuEvidenceSample& out) {
    out.core_c = GetDoubleEnvOrDefault("SVG_MB_CONTROL_SIM_GPU_CORE_C", 62.5);
    out.memjn_c = GetDoubleEnvOrDefault("SVG_MB_CONTROL_SIM_GPU_MEMJN_C", 72.25);
    out.hotspot_c = GetDoubleEnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_HOTSPOT_C", 80.75);
    out.nvml_temp_c = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_NVML_TEMP_C", 63u);
}

void FillSimGpuClocks(GpuEvidenceSample& out) {
    out.clock_graphics_mhz = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_CLOCK_GRAPHICS_MHZ", 2500u);
    out.clock_memory_mhz = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_CLOCK_MEMORY_MHZ", 10500u);
    out.clock_video_mhz = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_CLOCK_VIDEO_MHZ", 1800u);
    out.clock_boost_mhz = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_CLOCK_BOOST_MHZ", 2700u);
    out.nvml_clock_graphics_mhz = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_NVML_CLOCK_GRAPHICS_MHZ", 2490u);
    out.nvml_clock_memory_mhz = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_NVML_CLOCK_MEMORY_MHZ", 10490u);
    out.nvml_clock_video_mhz = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_NVML_CLOCK_VIDEO_MHZ", 1790u);
}

void FillSimGpuUtilization(GpuEvidenceSample& out) {
    out.pstate = GetInt32EnvOrDefault("SVG_MB_CONTROL_SIM_GPU_PSTATE", 0);
    out.util_gpu_pct = GetInt32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_UTIL_GPU_PCT", 67);
    out.util_fb_pct = GetInt32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_UTIL_FB_PCT", 21);
    out.util_vid_pct = GetInt32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_UTIL_VID_PCT", 4);
    out.nvml_util_gpu_pct = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_NVML_UTIL_GPU_PCT", 68u);
    out.nvml_util_mem_pct = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_NVML_UTIL_MEM_PCT", 22u);
    out.nvml_encoder_pct = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_NVML_ENCODER_PCT", 3u);
    out.nvml_decoder_pct = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_NVML_DECODER_PCT", 2u);
}

void FillSimGpuMemoryAndPower(GpuEvidenceSample& out) {
    out.vram_used_mb = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_VRAM_USED_MB", 8192u);
    out.vram_free_mb = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_VRAM_FREE_MB", 8192u);
    out.vram_total_mb = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_VRAM_TOTAL_MB", 16384u);
    out.nvml_power_mw = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_NVML_POWER_MW", 275000u);
    out.power_source = GetEnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_POWER_SOURCE", "nvml");
    out.voltage_core_mv = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_VOLTAGE_CORE_MV", 975u);
    out.pcie_tx_kb_s = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_PCIE_TX_KB_S", 1024u);
    out.pcie_rx_kb_s = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_PCIE_RX_KB_S", 2048u);
    out.pcie_link_gen = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_PCIE_LINK_GEN", 5u);
    out.pcie_link_width = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_PCIE_LINK_WIDTH", 16u);
    out.throttle_reasons = GetUint64EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_THROTTLE_REASONS", 8u);
}

void FillSimGpuDeviceCollections(GpuEvidenceSample& out) {
    out.fan_count = std::min<std::uint32_t>(
        GetUint32EnvOrDefault("SVG_MB_CONTROL_SIM_GPU_FAN_COUNT", 1u),
        static_cast<std::uint32_t>(kGpuEvidenceMaxFans));
    if (out.fan_count > 0u) {
        out.fans[0].level_pct = GetUint32EnvOrDefault(
            "SVG_MB_CONTROL_SIM_GPU_FAN0_LEVEL_PCT", 44u);
        out.fans[0].rpm = GetUint32EnvOrDefault(
            "SVG_MB_CONTROL_SIM_GPU_FAN0_RPM", 1550u);
    }

    out.power_rail_count = std::min<std::uint32_t>(
        GetUint32EnvOrDefault("SVG_MB_CONTROL_SIM_GPU_POWER_RAIL_COUNT", 1u),
        static_cast<std::uint32_t>(kGpuEvidenceMaxPowerRails));
    if (out.power_rail_count > 0u) {
        out.power_rails[0].domain = GetUint32EnvOrDefault(
            "SVG_MB_CONTROL_SIM_GPU_POWER_RAIL0_DOMAIN", 0u);
        out.power_rails[0].power_mw = GetUint32EnvOrDefault(
            "SVG_MB_CONTROL_SIM_GPU_POWER_RAIL0_POWER_MW", 123000u);
    }

    out.thermal_slot_count = std::min<std::uint32_t>(
        GetUint32EnvOrDefault("SVG_MB_CONTROL_SIM_GPU_THERMAL_SLOT_COUNT", 2u),
        static_cast<std::uint32_t>(kGpuEvidenceMaxThermalSlots));
    if (out.thermal_slot_count > 0u) {
        out.thermal_slots[0] = GetInt32EnvOrDefault(
            "SVG_MB_CONTROL_SIM_GPU_THERMAL_SLOT0_RAW", 16000);
    }
    if (out.thermal_slot_count > 1u) {
        out.thermal_slots[1] = GetInt32EnvOrDefault(
            "SVG_MB_CONTROL_SIM_GPU_THERMAL_SLOT1_RAW", 18496);
    }
}

GpuEvidenceSample MakeSimGpuEvidenceSample(std::string_view mode_label) {
    std::string mode_warning;
    GpuEvidenceSample out;
    out.available = true;
    out.requested_sample_mode = EffectiveRequestedModeLabel(mode_label);
    out.sample_mode =
        ResolveGpuEvidenceSampleModeLabel(mode_label, &mode_warning);
    out.detail = mode_warning.empty() ? "simulated GPU evidence captured"
                                      : mode_warning;
    out.gpu_name = GetEnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_NAME", "Simulated NVIDIA GPU");
    out.gpu_index = static_cast<int>(
        GetInt32EnvOrDefault("SVG_MB_CONTROL_SIM_GPU_INDEX", 0));
    out.time_ms = GetInt64EnvOrDefault("SVG_MB_CONTROL_SIM_GPU_TIME_MS", 1234);
    out.dt_ms = GetInt64EnvOrDefault("SVG_MB_CONTROL_SIM_GPU_DT_MS", 16);

    FillSimGpuTemperatures(out);
    FillSimGpuClocks(out);
    FillSimGpuUtilization(out);
    FillSimGpuMemoryAndPower(out);
    FillSimGpuDeviceCollections(out);
    return out;
}

#ifdef SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED
std::uint32_t PreferNonZero(std::uint32_t primary, std::uint32_t fallback) {
    return primary != 0u ? primary : fallback;
}

CachedGpuContext BuildCachedGpuContext(const GpuSnapshot& fast,
                                       bool fast_ok,
                                       const GpuSnapshot& rare,
                                       bool rare_ok,
                                       std::uint64_t sample_id,
                                       std::chrono::steady_clock::time_point now) {
    CachedGpuContext next;
    if (!fast_ok && !rare_ok) {
        return next;
    }
    next.sample_id = sample_id;
    next.sampled_at = now;
    next.time_ms = static_cast<double>(MonotonicGpuReadMs());
    if (fast_ok) {
        next.util_gpu_pct = fast.util_gpu_pct;
        next.util_mem_pct = fast.util_fb_pct;
        next.pstate = fast.pstate;
        next.clock_graphics_mhz = PreferNonZero(
            fast.nvml_clock_graphics_mhz,
            fast.clock_graphics_mhz);
        next.clock_memory_mhz = PreferNonZero(
            fast.nvml_clock_memory_mhz,
            fast.clock_memory_mhz);
    }
    if (rare_ok && rare.vram_total_mb != 0u) {
        next.vram_used_mb = rare.vram_used_mb;
        next.vram_total_mb = rare.vram_total_mb;
    }
    next.valid = CachedGpuContextHasValue(next);
    return next;
}

GpuSampleMode ToGpuSampleMode(std::string_view mode_label) {
    if (mode_label == "thermal-fast") {
        return GpuSampleMode::ThermalFast;
    }
    if (mode_label == "fast") {
        return GpuSampleMode::Fast;
    }
    if (mode_label == "medium") {
        return GpuSampleMode::Medium;
    }
    if (mode_label == "slow") {
        return GpuSampleMode::Slow;
    }
    if (mode_label == "rare") {
        return GpuSampleMode::Rare;
    }
    return GpuSampleMode::Full;
}

GpuEvidenceSample ConvertGpuEvidenceSnapshot(
    const GpuSnapshot& snapshot,
    std::string sample_mode,
    std::string requested_sample_mode,
    std::string detail,
    std::string gpu_name) {
    GpuEvidenceSample out;
    out.available = true;
    out.sample_mode = std::move(sample_mode);
    out.requested_sample_mode = std::move(requested_sample_mode);
    out.detail = std::move(detail);
    out.gpu_name = std::move(gpu_name);

    out.gpu_index = snapshot.gpu_index;
    out.time_ms = snapshot.time_ms;
    out.dt_ms = snapshot.dt_ms;
    out.core_c = snapshot.core_c;
    out.memjn_c = snapshot.memjn_c;
    out.hotspot_c = snapshot.hotspot_c;
    out.nvml_temp_c = snapshot.nvml_temp_c;
    out.clock_graphics_mhz = snapshot.clock_graphics_mhz;
    out.clock_memory_mhz = snapshot.clock_memory_mhz;
    out.clock_video_mhz = snapshot.clock_video_mhz;
    out.clock_boost_mhz = snapshot.clock_boost_mhz;
    out.nvml_clock_graphics_mhz = snapshot.nvml_clock_graphics_mhz;
    out.nvml_clock_memory_mhz = snapshot.nvml_clock_memory_mhz;
    out.nvml_clock_video_mhz = snapshot.nvml_clock_video_mhz;
    out.pstate = snapshot.pstate;
    out.util_gpu_pct = snapshot.util_gpu_pct;
    out.util_fb_pct = snapshot.util_fb_pct;
    out.util_vid_pct = snapshot.util_vid_pct;
    out.nvml_util_gpu_pct = snapshot.nvml_util_gpu_pct;
    out.nvml_util_mem_pct = snapshot.nvml_util_mem_pct;
    out.nvml_encoder_pct = snapshot.nvml_encoder_pct;
    out.nvml_decoder_pct = snapshot.nvml_decoder_pct;
    out.vram_used_mb = snapshot.vram_used_mb;
    out.vram_free_mb = snapshot.vram_free_mb;
    out.vram_total_mb = snapshot.vram_total_mb;

    out.fan_count = static_cast<std::uint32_t>(
        std::clamp(snapshot.fan_count,
                   0,
                   static_cast<int>(kGpuEvidenceMaxFans)));
    for (std::uint32_t index = 0u; index < out.fan_count; ++index) {
        out.fans[index].level_pct = snapshot.fans[index].level_pct;
        out.fans[index].rpm = snapshot.fans[index].rpm;
    }

    out.nvml_power_mw = snapshot.nvml_power_mw;
    out.power_source = gpu_power_source_name(snapshot.power_source);
    out.power_rail_count = static_cast<std::uint32_t>(
        std::clamp(snapshot.power_rail_count,
                   0,
                   static_cast<int>(kGpuEvidenceMaxPowerRails)));
    for (std::uint32_t index = 0u; index < out.power_rail_count; ++index) {
        out.power_rails[index].domain = snapshot.power_rails[index].domain;
        out.power_rails[index].power_mw = snapshot.power_rails[index].power_mw;
    }

    out.voltage_core_mv = snapshot.voltage_core_mv;
    out.pcie_tx_kb_s = snapshot.pcie_tx_kb_s;
    out.pcie_rx_kb_s = snapshot.pcie_rx_kb_s;
    out.pcie_link_gen = snapshot.pcie_link_gen;
    out.pcie_link_width = snapshot.pcie_link_width;
    out.throttle_reasons = snapshot.throttle_reasons;

    out.thermal_slot_count = static_cast<std::uint32_t>(
        std::clamp(snapshot.thermal_slot_count,
                   0,
                   static_cast<int>(kGpuEvidenceMaxThermalSlots)));
    for (std::uint32_t index = 0u; index < out.thermal_slot_count; ++index) {
        out.thermal_slots[index] = snapshot.thermal_slots[index];
    }
    return out;
}
#endif

}  // namespace

#ifdef SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED

struct GpuReader::Impl {
    GpuSensorReader reader;
    bool initialized = false;
    std::string init_warning;
    std::string gpu_name;
    GpuTempSample last_sample;
    // FEAT-0020 GPU board-power read counter (advances only on a fresh nonzero
    // read; see FinalizeGpuPowerIdentity).
    std::uint64_t power_sample_counter = 0u;
    // FEAT-0021 cached workload-context sample counter. The context cache is
    // refreshed at most once per kGpuContextRefreshInterval; rows in between
    // mirror the same sample id with an increasing age.
    std::uint64_t context_sample_counter = 0u;
    CachedGpuContext context_cache;
};

GpuReader::GpuReader() : impl_(std::make_unique<Impl>()) {
    std::string warning;
    const bool ok = impl_->reader.init(warning);
    impl_->initialized = ok;
    impl_->init_warning = warning;
    if (ok && impl_->reader.gpu_count() > 0) {
        if (const GpuInfo* info = impl_->reader.gpu_info(0)) {
            impl_->gpu_name = info->name;
        }
    }
}

GpuReader::~GpuReader() {
    if (impl_ && impl_->initialized) {
        impl_->reader.shutdown();
    }
}

bool GpuReader::available() const {
    return impl_ != nullptr && impl_->initialized &&
           impl_->reader.gpu_count() > 0;
}

std::string GpuReader::init_warning() const {
    return impl_ ? impl_->init_warning : std::string();
}

const GpuTempSample& GpuReader::Sample() {
    const auto sample_started = std::chrono::steady_clock::now();
    GpuTempSample& out = impl_->last_sample;
    out.available = false;
    out.core_c = 0.0;
    out.memjn_c = 0.0;
    out.hotspot_c = 0.0;
    out.gpu_name.clear();
    out.last_warning.clear();
    // FEAT-0020: default to no live power reading; the success path below sets
    // "nvml". last_sample is reused across ticks, so reset these explicitly.
    out.power_acquisition = "unavailable";
    out.power_source = "unknown";
    out.power_mw = std::numeric_limits<double>::quiet_NaN();
    ResetGpuContextFields(out, "unavailable");

    if (SimGpuEnabled()) {
        out = MakeSimGpuTempSample();
        FinalizeGpuPowerIdentity(out, impl_->power_sample_counter);
        FinalizeGpuContextIdentity(out, impl_->context_sample_counter);
        return out;
    }
    if (SimGpuUnavailable()) {
        out.last_warning = "GPU reader disabled by environment";
        FinalizeGpuPowerIdentity(out, impl_->power_sample_counter);
        ResetGpuContextFields(out, "unavailable");
        return out;
    }

    if (!available()) {
        out.last_warning = impl_->init_warning.empty()
            ? std::string("not initialized")
            : impl_->init_warning;
        FinalizeGpuPowerIdentity(out, impl_->power_sample_counter);
        ResetGpuContextFields(out, "unavailable");
        return out;
    }
    GpuSnapshot snap;
    if (!impl_->reader.sample(0, snap, GpuSampleMode::ThermalFast)) {
        out.last_warning = "sample failed";
        FinalizeGpuPowerIdentity(out, impl_->power_sample_counter);
        ApplyCachedGpuContext(out, impl_->context_cache, sample_started,
                              "unavailable");
        return out;
    }
    out.available = true;
    out.core_c = snap.core_c;
    out.memjn_c = snap.memjn_c;
    out.hotspot_c = snap.hotspot_c;
    out.gpu_name = impl_->gpu_name;
    // FEAT-0020 board power from the same thermal-fast sample (the board-power-only
    // poll_nvml_board_power read is included in sample_thermal_fast; the per-rail
    // poll_power topology is NOT on this hot path). nonzero-gated: a zero reading
    // means no live board power (no false zero), so it stays "unavailable".
    if (snap.nvml_power_mw != 0u) {
        out.power_mw = static_cast<double>(snap.nvml_power_mw);
        out.power_source = gpu_power_source_name(snap.power_source);
        out.power_acquisition = "nvml";
    }
    FinalizeGpuPowerIdentity(out, impl_->power_sample_counter);
    const bool context_due =
        !impl_->context_cache.valid ||
        (sample_started - impl_->context_cache.sampled_at) >=
            kGpuContextRefreshInterval;
    if (context_due) {
        GpuSnapshot fast;
        GpuSnapshot rare;
        const bool fast_ok =
            impl_->reader.sample(0, fast, GpuSampleMode::Fast);
        const bool rare_ok =
            impl_->reader.sample(0, rare, GpuSampleMode::Rare);
        CachedGpuContext next = BuildCachedGpuContext(
            fast, fast_ok, rare, rare_ok,
            impl_->context_sample_counter + 1u,
            std::chrono::steady_clock::now());
        if (next.valid) {
            ++impl_->context_sample_counter;
            impl_->context_cache = next;
        }
    }
    ApplyCachedGpuContext(out, impl_->context_cache,
                          std::chrono::steady_clock::now(),
                          "unavailable");
    return out;
}

GpuEvidenceSample GpuReader::SampleEvidence(std::string_view mode_label) {
    if (SimGpuEnabled()) {
        return MakeSimGpuEvidenceSample(mode_label);
    }
    if (SimGpuUnavailable()) {
        GpuEvidenceSample out;
        out.available = false;
        out.requested_sample_mode = EffectiveRequestedModeLabel(mode_label);
        out.sample_mode = out.requested_sample_mode;
        out.detail = "GPU reader disabled by environment";
        return out;
    }

    std::string mode_warning;
    const std::string resolved_mode =
        ResolveGpuEvidenceSampleModeLabel(mode_label, &mode_warning);

    GpuEvidenceSample out;
    out.requested_sample_mode = EffectiveRequestedModeLabel(mode_label);
    out.sample_mode = resolved_mode;
    if (!available()) {
        out.last_warning = impl_ ? impl_->init_warning : "not initialized";
        out.detail = out.last_warning;
        return out;
    }

    GpuSnapshot snap;
    if (!impl_->reader.sample(0, snap, ToGpuSampleMode(resolved_mode))) {
        out.last_warning = "sample failed";
        out.detail = out.last_warning;
        return out;
    }

    return ConvertGpuEvidenceSnapshot(
        snap,
        resolved_mode,
        out.requested_sample_mode,
        mode_warning.empty() ? "GPU evidence captured" : mode_warning,
        impl_->gpu_name);
}

#else  // SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED

struct GpuReader::Impl {
    std::string init_warning =
        "gpu_telemetry not linked at build time";
    GpuTempSample last_sample;
    std::uint64_t power_sample_counter = 0u;
    std::uint64_t context_sample_counter = 0u;
};

GpuReader::GpuReader() : impl_(std::make_unique<Impl>()) {}
GpuReader::~GpuReader() = default;

bool GpuReader::available() const { return false; }

std::string GpuReader::init_warning() const {
    return impl_ ? impl_->init_warning : std::string();
}

const GpuTempSample& GpuReader::Sample() {
    GpuTempSample& out = impl_->last_sample;
    out.available = false;
    out.core_c = 0.0;
    out.memjn_c = 0.0;
    out.hotspot_c = 0.0;
    out.gpu_name.clear();
    out.last_warning.clear();
    // FEAT-0020: GPU telemetry is not built into this binary, so board power is
    // structurally "disabled" (distinct from an enabled-build "unavailable").
    out.power_acquisition = "disabled";
    out.power_source = "unknown";
    out.power_mw = std::numeric_limits<double>::quiet_NaN();
    ResetGpuContextFields(out, "disabled");

    if (SimGpuEnabled()) {
        out = MakeSimGpuTempSample();
        FinalizeGpuPowerIdentity(out, impl_->power_sample_counter);
        FinalizeGpuContextIdentity(out, impl_->context_sample_counter);
        return out;
    }
    if (SimGpuUnavailable()) {
        out.last_warning = "GPU reader disabled by environment";
        out.power_acquisition = "unavailable";
        FinalizeGpuPowerIdentity(out, impl_->power_sample_counter);
        ResetGpuContextFields(out, "unavailable");
        return out;
    }

    out.last_warning = init_warning();
    FinalizeGpuPowerIdentity(out, impl_->power_sample_counter);
    ResetGpuContextFields(out, "disabled");
    return out;
}

GpuEvidenceSample GpuReader::SampleEvidence(std::string_view mode_label) {
    if (SimGpuEnabled()) {
        return MakeSimGpuEvidenceSample(mode_label);
    }
    if (SimGpuUnavailable()) {
        GpuEvidenceSample out;
        out.available = false;
        out.requested_sample_mode = EffectiveRequestedModeLabel(mode_label);
        out.sample_mode = out.requested_sample_mode;
        out.detail = "GPU reader disabled by environment";
        return out;
    }

    std::string mode_warning;
    GpuEvidenceSample out;
    out.requested_sample_mode = EffectiveRequestedModeLabel(mode_label);
    out.sample_mode =
        ResolveGpuEvidenceSampleModeLabel(mode_label, &mode_warning);
    out.last_warning = init_warning();
    out.detail = mode_warning.empty() ? out.last_warning : mode_warning;
    return out;
}

#endif  // SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED

}  // namespace svg_mb_control
