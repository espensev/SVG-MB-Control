#include "direct_runtime_snapshot.h"

#include "amd_reader.h"
#include "fan_writer.h"
#include "gpu_reader.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>

namespace svg_mb_control {

namespace {

std::string GetEnvOrDefault(const char* name, std::string_view fallback) {
    char* value = nullptr;
    std::size_t size = 0u;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr ||
        value[0] == '\0') {
        if (value != nullptr) {
            std::free(value);
        }
        return std::string(fallback);
    }
    std::string result(value);
    std::free(value);
    return result;
}

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

std::string FormatLocalIso8601(std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm local{};
    if (localtime_s(&local, &tt) != 0) {
        return {};
    }
    std::array<char, 32> buffer{};
    const std::size_t written = std::strftime(buffer.data(), buffer.size(),
                                              "%Y-%m-%dT%H:%M:%S", &local);
    return written > 0u ? std::string(buffer.data(), written) : std::string();
}

void MergeAmdTelemetry(RuntimeSnapshot& snapshot,
                       const AmdSnapshot& amd_snapshot) {
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
}

void MergeFanTelemetry(RuntimeSnapshot& snapshot,
                       const FanScanResult& scan_result,
                       const RuntimeWritePolicy& runtime_policy) {
    if (!scan_result) {
        return;
    }
    bool sim_write_allowed = true;
    const bool has_sim_write_allowed = TryParseBoolEnv(
        "SVG_MB_CONTROL_SIM_WRITE_ALLOWED", &sim_write_allowed);
    bool sim_policy_blocked = false;
    const bool has_sim_policy_blocked = TryParseBoolEnv(
        "SVG_MB_CONTROL_SIM_POLICY_BLOCKED", &sim_policy_blocked);
    bool sim_effective_write_allowed = true;
    const bool has_sim_effective_write_allowed = TryParseBoolEnv(
        "SVG_MB_CONTROL_SIM_EFFECTIVE_WRITE_ALLOWED",
        &sim_effective_write_allowed);

    for (const auto& fan_state : scan_result.fans) {
        RuntimeFanSnapshot& fan =
            UpsertRuntimeFanChannel(snapshot, fan_state.channel);
        fan.label = fan_state.label;
        fan.rpm = fan_state.rpm;
        fan.tach_raw = fan_state.tach_raw;
        fan.duty_raw = fan_state.duty_raw;
        fan.mode_raw = fan_state.mode_raw;
        fan.duty_percent = fan_state.duty_percent;
        fan.tach_valid = fan_state.tach_valid;
        fan.manual_override = fan_state.manual_override;
        fan.write_allowed = has_sim_write_allowed ? sim_write_allowed : true;
        fan.policy_blocked = has_sim_policy_blocked
            ? sim_policy_blocked
            : RuntimeWritePolicyBlocksChannel(runtime_policy,
                                              fan_state.channel);
        fan.effective_write_allowed = has_sim_effective_write_allowed
            ? sim_effective_write_allowed
            : (fan.write_allowed && runtime_policy.writes_enabled &&
               !fan.policy_blocked);
    }
}

}  // namespace

RuntimeSnapshot SampleDirectRuntimeSnapshot(
    AmdReader& amd_reader,
    GpuReader& gpu_reader,
    FanWriter& fan_writer,
    const RuntimeWritePolicy& runtime_policy) {
    RuntimeSnapshot snapshot;
    const auto snapshot_offset_ms = GetLongEnvOrDefault(
        "SVG_MB_CONTROL_SIM_SNAPSHOT_OFFSET_MS", 0);
    snapshot.snapshot_time_iso = FormatLocalIso8601(
        std::chrono::system_clock::now() -
        std::chrono::milliseconds(snapshot_offset_ms));
    snapshot.policy_writes_enabled_present = runtime_policy.present;
    snapshot.policy_writes_enabled = runtime_policy.writes_enabled;
    bool sim_policy_writes_enabled = false;
    if (TryParseBoolEnv("SVG_MB_CONTROL_SIM_POLICY_WRITES_ENABLED",
                        &sim_policy_writes_enabled)) {
        snapshot.policy_writes_enabled_present = true;
        snapshot.policy_writes_enabled = sim_policy_writes_enabled;
    }

    MergeAmdTelemetry(snapshot, amd_reader.Sample());
    MergeGpuTelemetry(snapshot, gpu_reader.Sample());
    MergeFanTelemetry(snapshot, fan_writer.ReadAllChannels(), runtime_policy);
    return snapshot;
}

bool RuntimeSnapshotHasTelemetry(const RuntimeSnapshot& snapshot) {
    return !snapshot.amd_sensors.empty() ||
           !snapshot.fans.empty() ||
           snapshot.gpu.available ||
           !snapshot.gpu.last_warning.empty();
}

}  // namespace svg_mb_control
