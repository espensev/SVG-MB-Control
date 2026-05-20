#include "gpu_reader.h"

#ifdef SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED
#include "gpu_telemetry/gpu_sensor_reader.h"
#endif

#include "env_util.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>
#include <utility>

namespace svg_mb_control {

namespace {

bool SimGpuEnabled() {
    const std::string mode = GetEnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_MODE", "disabled");
    return mode == "enabled" || mode == "1" || mode == "true" ||
           mode == "TRUE" || mode == "True";
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
    try {
        return std::stod(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::uint32_t GetUint32EnvOrDefault(const char* name,
                                    std::uint32_t fallback) {
    const std::string value = GetEnvOrDefault(name, "");
    if (value.empty()) {
        return fallback;
    }
    try {
        return static_cast<std::uint32_t>(std::stoul(value));
    } catch (const std::exception&) {
        return fallback;
    }
}

std::int32_t GetInt32EnvOrDefault(const char* name, std::int32_t fallback) {
    const std::string value = GetEnvOrDefault(name, "");
    if (value.empty()) {
        return fallback;
    }
    try {
        return static_cast<std::int32_t>(std::stol(value));
    } catch (const std::exception&) {
        return fallback;
    }
}

std::uint64_t GetUint64EnvOrDefault(const char* name,
                                    std::uint64_t fallback) {
    const std::string value = GetEnvOrDefault(name, "");
    if (value.empty()) {
        return fallback;
    }
    try {
        return static_cast<std::uint64_t>(std::stoull(value));
    } catch (const std::exception&) {
        return fallback;
    }
}

std::int64_t GetInt64EnvOrDefault(const char* name, std::int64_t fallback) {
    const std::string value = GetEnvOrDefault(name, "");
    if (value.empty()) {
        return fallback;
    }
    try {
        return static_cast<std::int64_t>(std::stoll(value));
    } catch (const std::exception&) {
        return fallback;
    }
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
    return out;
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

    out.core_c = GetDoubleEnvOrDefault("SVG_MB_CONTROL_SIM_GPU_CORE_C", 62.5);
    out.memjn_c = GetDoubleEnvOrDefault("SVG_MB_CONTROL_SIM_GPU_MEMJN_C", 72.25);
    out.hotspot_c = GetDoubleEnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_HOTSPOT_C", 80.75);
    out.nvml_temp_c = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_NVML_TEMP_C", 63u);

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

    out.vram_used_mb = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_VRAM_USED_MB", 8192u);
    out.vram_free_mb = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_VRAM_FREE_MB", 8192u);
    out.vram_total_mb = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_VRAM_TOTAL_MB", 16384u);

    out.fan_count = std::min<std::uint32_t>(
        GetUint32EnvOrDefault("SVG_MB_CONTROL_SIM_GPU_FAN_COUNT", 1u),
        static_cast<std::uint32_t>(kGpuEvidenceMaxFans));
    if (out.fan_count > 0u) {
        out.fans[0].level_pct = GetUint32EnvOrDefault(
            "SVG_MB_CONTROL_SIM_GPU_FAN0_LEVEL_PCT", 44u);
        out.fans[0].rpm = GetUint32EnvOrDefault(
            "SVG_MB_CONTROL_SIM_GPU_FAN0_RPM", 1550u);
    }

    out.nvml_power_mw = GetUint32EnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_NVML_POWER_MW", 275000u);
    out.power_source = GetEnvOrDefault(
        "SVG_MB_CONTROL_SIM_GPU_POWER_SOURCE", "nvml");
    out.power_rail_count = std::min<std::uint32_t>(
        GetUint32EnvOrDefault("SVG_MB_CONTROL_SIM_GPU_POWER_RAIL_COUNT", 1u),
        static_cast<std::uint32_t>(kGpuEvidenceMaxPowerRails));
    if (out.power_rail_count > 0u) {
        out.power_rails[0].domain = GetUint32EnvOrDefault(
            "SVG_MB_CONTROL_SIM_GPU_POWER_RAIL0_DOMAIN", 0u);
        out.power_rails[0].power_mw = GetUint32EnvOrDefault(
            "SVG_MB_CONTROL_SIM_GPU_POWER_RAIL0_POWER_MW", 123000u);
    }

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
    return out;
}

#ifdef SVG_MB_CONTROL_GPU_TELEMETRY_ENABLED
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
    GpuTempSample& out = impl_->last_sample;
    out.available = false;
    out.core_c = 0.0;
    out.memjn_c = 0.0;
    out.hotspot_c = 0.0;
    out.gpu_name.clear();
    out.last_warning.clear();

    if (SimGpuEnabled()) {
        out = MakeSimGpuTempSample();
        return out;
    }

    if (!available()) {
        out.last_warning = impl_->init_warning.empty()
            ? std::string("not initialized")
            : impl_->init_warning;
        return out;
    }
    GpuSnapshot snap;
    if (!impl_->reader.sample(0, snap, GpuSampleMode::ThermalFast)) {
        out.last_warning = "sample failed";
        return out;
    }
    out.available = true;
    out.core_c = snap.core_c;
    out.memjn_c = snap.memjn_c;
    out.hotspot_c = snap.hotspot_c;
    out.gpu_name = impl_->gpu_name;
    return out;
}

GpuEvidenceSample GpuReader::SampleEvidence(std::string_view mode_label) {
    if (SimGpuEnabled()) {
        return MakeSimGpuEvidenceSample(mode_label);
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

    if (SimGpuEnabled()) {
        out = MakeSimGpuTempSample();
        return out;
    }

    out.last_warning = init_warning();
    return out;
}

GpuEvidenceSample GpuReader::SampleEvidence(std::string_view mode_label) {
    if (SimGpuEnabled()) {
        return MakeSimGpuEvidenceSample(mode_label);
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
