#include "direct_runtime_snapshot.h"

#include "amd_reader.h"
#include "control_scheduler.h"
#include "env_util.h"
#include "fan_writer.h"
#include "gpu_reader.h"

#include <array>
#include <chrono>
#include <ctime>
#include <string>
#include <string_view>

namespace svg_mb_control {

namespace {

bool TryParseBoolEnv(const char* name, bool* out_value) {
    const std::string value = GetEnvOrDefault(name, "");
    if (value.empty()) {
        return false;
    }
    if (value == "1" || value == "true" || value == "TRUE" ||
        value == "True") {
        *out_value = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "FALSE" ||
        value == "False") {
        *out_value = false;
        return true;
    }
    return false;
}

long GetLongEnvOrDefault(const char* name, long fallback) {
    const std::string value = GetEnvOrDefault(name, "");
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stol(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

struct SimRuntimeOverrides {
    bool has_write_allowed = false;
    bool write_allowed = true;
    bool has_policy_blocked = false;
    bool policy_blocked = false;
    bool has_effective_write_allowed = false;
    bool effective_write_allowed = true;
    bool has_policy_writes_enabled = false;
    bool policy_writes_enabled = false;
    long snapshot_offset_ms = 0;
};

const SimRuntimeOverrides& CachedSimRuntimeOverrides() {
    static const SimRuntimeOverrides overrides = [] {
        SimRuntimeOverrides value;
        value.has_write_allowed = TryParseBoolEnv(
            "SVG_MB_CONTROL_SIM_WRITE_ALLOWED", &value.write_allowed);
        value.has_policy_blocked = TryParseBoolEnv(
            "SVG_MB_CONTROL_SIM_POLICY_BLOCKED", &value.policy_blocked);
        value.has_effective_write_allowed = TryParseBoolEnv(
            "SVG_MB_CONTROL_SIM_EFFECTIVE_WRITE_ALLOWED",
            &value.effective_write_allowed);
        value.has_policy_writes_enabled = TryParseBoolEnv(
            "SVG_MB_CONTROL_SIM_POLICY_WRITES_ENABLED",
            &value.policy_writes_enabled);
        value.snapshot_offset_ms = GetLongEnvOrDefault(
            "SVG_MB_CONTROL_SIM_SNAPSHOT_OFFSET_MS", 0);
        return value;
    }();
    return overrides;
}

void MergeAmdTelemetry(RuntimeSnapshot& snapshot,
                       const AmdSnapshot& amd_snapshot) {
    // FEAT-0006 package energy is independent of temperature availability, so
    // copy it before the temperature early-return. AmdReader resets these every
    // Sample(), so nothing stale carries over when AMD is unavailable.
    snapshot.pkg_energy_acquisition = amd_snapshot.pkg_energy_acquisition;
    snapshot.pkg_energy_sample_id = amd_snapshot.pkg_energy_sample_id;
    snapshot.pkg_energy_window_ms = amd_snapshot.pkg_energy_window_ms;
    snapshot.pkg_energy_delta_uj = amd_snapshot.pkg_energy_delta_uj;
    snapshot.cpu_cycles_acquisition = amd_snapshot.cpu_cycles_acquisition;
    snapshot.cpu_cycles_sample_id = amd_snapshot.cpu_cycles_sample_id;
    snapshot.cpu_cycles_window_ms = amd_snapshot.cpu_cycles_window_ms;
    snapshot.cpu_aperf_delta = amd_snapshot.cpu_aperf_delta;
    snapshot.cpu_mperf_delta = amd_snapshot.cpu_mperf_delta;
    if (!amd_snapshot.available || amd_snapshot.samples.empty()) {
        return;
    }
    snapshot.amd_sensors.clear();
    snapshot.amd_sensors.reserve(amd_snapshot.samples.size());
    for (const auto& sample : amd_snapshot.samples) {
        snapshot.amd_sensors.push_back(
            RuntimeAmdSensor{sample.label, sample.temperature_c});
    }
}

void MergeGpuTelemetry(RuntimeSnapshot& snapshot,
                       const GpuTempSample& gpu_sample) {
    snapshot.gpu.available = gpu_sample.available;
    snapshot.gpu.core_c = gpu_sample.core_c;
    snapshot.gpu.memjn_c = gpu_sample.memjn_c;
    snapshot.gpu.hotspot_c = gpu_sample.hotspot_c;
    snapshot.gpu.gpu_name = gpu_sample.gpu_name;
    snapshot.gpu.last_warning = gpu_sample.last_warning;
    // FEAT-0020 read-only GPU board power (logging-only; never a control input).
    snapshot.gpu.power_acquisition = gpu_sample.power_acquisition;
    snapshot.gpu.power_source = gpu_sample.power_source;
    snapshot.gpu.power_sample_id = gpu_sample.power_sample_id;
    snapshot.gpu.power_time_ms = gpu_sample.power_time_ms;
    snapshot.gpu.power_mw = gpu_sample.power_mw;
    // FEAT-0021 read-only GPU workload context (logging-only; never a control
    // input). Values are cached by GpuReader and mirrored into the control CSV
    // with an explicit sample age.
    snapshot.gpu.context_acquisition = gpu_sample.context_acquisition;
    snapshot.gpu.context_sample_id = gpu_sample.context_sample_id;
    snapshot.gpu.context_time_ms = gpu_sample.context_time_ms;
    snapshot.gpu.context_sample_age_ms = gpu_sample.context_sample_age_ms;
    snapshot.gpu.context_util_gpu_pct = gpu_sample.context_util_gpu_pct;
    snapshot.gpu.context_util_mem_pct = gpu_sample.context_util_mem_pct;
    snapshot.gpu.context_pstate = gpu_sample.context_pstate;
    snapshot.gpu.context_clock_graphics_mhz =
        gpu_sample.context_clock_graphics_mhz;
    snapshot.gpu.context_clock_memory_mhz =
        gpu_sample.context_clock_memory_mhz;
    snapshot.gpu.context_vram_used_mb = gpu_sample.context_vram_used_mb;
    snapshot.gpu.context_vram_total_mb = gpu_sample.context_vram_total_mb;
}

void MergeFanTelemetry(RuntimeSnapshot& snapshot,
                       const FanScanResult& scan_result,
                       const RuntimeWritePolicy& runtime_policy) {
    if (!scan_result) {
        return;
    }
    const SimRuntimeOverrides& sim = CachedSimRuntimeOverrides();

    for (const auto& fan_state : scan_result.fans) {
        RuntimeFanSnapshot& fan =
            UpsertRuntimeFanChannel(snapshot, fan_state.channel);
        fan.label = fan_state.label;
        fan.rpm = fan_state.rpm;
        fan.tach_raw = fan_state.tach_raw;
        fan.tach_hi_raw = fan_state.tach_hi_raw;
        fan.tach_lo_raw = fan_state.tach_lo_raw;
        fan.duty_raw = fan_state.duty_raw;
        fan.mode_raw = fan_state.mode_raw;
        fan.duty_percent = fan_state.duty_percent;
        fan.tach_valid = fan_state.tach_valid;
        fan.manual_override = fan_state.manual_override;
        fan.write_allowed = sim.has_write_allowed ? sim.write_allowed : true;
        fan.policy_blocked = sim.has_policy_blocked
            ? sim.policy_blocked
            : RuntimeWritePolicyBlocksChannel(runtime_policy,
                                              fan_state.channel);
        fan.effective_write_allowed = sim.has_effective_write_allowed
            ? sim.effective_write_allowed
            : (fan.write_allowed && runtime_policy.writes_enabled &&
               !fan.policy_blocked);
    }
}

}  // namespace

void SampleDirectRuntimeSnapshot(
    AmdReader& amd_reader,
    GpuReader& gpu_reader,
    FanWriter& fan_writer,
    const RuntimeWritePolicy& runtime_policy,
    RuntimeSnapshot& out) {
    SampleDirectRuntimeSnapshot(amd_reader, gpu_reader, fan_writer,
                                runtime_policy, out, nullptr);
}

void SampleDirectRuntimeSnapshot(
    AmdReader& amd_reader,
    GpuReader& gpu_reader,
    FanWriter& fan_writer,
    const RuntimeWritePolicy& runtime_policy,
    RuntimeSnapshot& out,
    DirectRuntimeSnapshotTiming* timing) {
    const auto total_started = std::chrono::steady_clock::now();
    if (timing != nullptr) {
        *timing = DirectRuntimeSnapshotTiming{};
    }

    // Reset before the merges. amd_sensors/fans must be cleared explicitly:
    // MergeAmdTelemetry early-returns without clearing when AMD is
    // unavailable, and MergeFanTelemetry upserts into fans, so a reused
    // snapshot would otherwise retain the previous tick's telemetry.
    // gpu and the policy/time scalars are overwritten unconditionally below
    // and by MergeGpuTelemetry, so they need no separate reset. clear()
    // keeps the vector buffers allocated across ticks.
    out.amd_sensors.clear();
    out.fans.clear();

    const SimRuntimeOverrides& sim = CachedSimRuntimeOverrides();
    out.snapshot_time_iso = FormatLocalIso8601(
        std::chrono::system_clock::now() -
        std::chrono::milliseconds(sim.snapshot_offset_ms));
    out.policy_writes_enabled_present = runtime_policy.present;
    out.policy_writes_enabled = runtime_policy.writes_enabled;
    if (sim.has_policy_writes_enabled) {
        out.policy_writes_enabled_present = true;
        out.policy_writes_enabled = sim.policy_writes_enabled;
    }

    const auto amd_started = std::chrono::steady_clock::now();
    const AmdSnapshot& amd_snapshot = amd_reader.Sample();
    if (timing != nullptr) {
        timing->amd_read_ms =
            DurationMilliseconds(std::chrono::steady_clock::now() -
                                 amd_started);
    }
    MergeAmdTelemetry(out, amd_snapshot);

    const auto gpu_started = std::chrono::steady_clock::now();
    const GpuTempSample& gpu_sample = gpu_reader.Sample();
    if (timing != nullptr) {
        timing->gpu_thermal_read_ms =
            DurationMilliseconds(std::chrono::steady_clock::now() -
                                 gpu_started);
    }
    MergeGpuTelemetry(out, gpu_sample);

    // Reused across calls on this thread; FanWriter::ReadAllChannels resets
    // out.error/out.detail and clear()s out.fans while retaining capacity, so
    // a previous sample's state does not bleed in. Each sampler thread
    // (control loop, read loop, evidence log) keeps its own buffer.
    thread_local FanScanResult fan_scan_scratch;
    const auto fan_scan_started = std::chrono::steady_clock::now();
    fan_writer.ReadAllChannels(fan_scan_scratch);
    if (timing != nullptr) {
        timing->fan_scan_read_ms =
            DurationMilliseconds(std::chrono::steady_clock::now() -
                                 fan_scan_started);
    }
    MergeFanTelemetry(out, fan_scan_scratch, runtime_policy);

    if (timing != nullptr) {
        timing->total_read_ms =
            DurationMilliseconds(std::chrono::steady_clock::now() -
                                 total_started);
    }
}

RuntimeSnapshot SampleDirectRuntimeSnapshot(
    AmdReader& amd_reader,
    GpuReader& gpu_reader,
    FanWriter& fan_writer,
    const RuntimeWritePolicy& runtime_policy) {
    RuntimeSnapshot snapshot;
    SampleDirectRuntimeSnapshot(amd_reader, gpu_reader, fan_writer,
                                runtime_policy, snapshot);
    return snapshot;
}

bool RuntimeSnapshotHasTelemetry(const RuntimeSnapshot& snapshot) {
    return !snapshot.amd_sensors.empty() ||
           !snapshot.fans.empty() ||
           snapshot.gpu.available ||
           !snapshot.gpu.last_warning.empty();
}

}  // namespace svg_mb_control
