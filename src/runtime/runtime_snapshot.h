#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace svg_mb_control {

struct RuntimeAmdSensor {
    std::string label;
    double temperature_c = 0.0;
};

struct RuntimeFanSnapshot {
    std::uint32_t channel = 0u;
    std::string label;
    std::uint16_t rpm = 0u;
    std::uint16_t tach_raw = 0u;
    std::uint8_t tach_hi_raw = 0u;
    std::uint8_t tach_lo_raw = 0u;
    std::uint8_t duty_raw = 0u;
    std::uint8_t mode_raw = 0u;
    double duty_percent = 0.0;
    bool tach_valid = false;
    bool manual_override = false;
    bool write_allowed = false;
    bool policy_blocked = false;
    bool effective_write_allowed = false;
};

struct RuntimeGpuSnapshot {
    bool available = false;
    double core_c = 0.0;
    double memjn_c = 0.0;
    double hotspot_c = 0.0;
    std::string gpu_name;
    std::string last_warning;

    // FEAT-0020 read-only GPU board power (REQ-PWRLOG-02), copied from the GPU
    // reader's per-tick thermal+power sample. Logging-only; never a control
    // input. Carried in-memory to the control-loop CSV; not serialized to the
    // snapshot JSON sidecar. acquisition: "disabled" | "unavailable" | "nvml".
    // sample_id advances only on a fresh nonzero NVML read (a repeated id marks
    // a stale/mirrored value); time_ms/mw are blank (NaN) when no live read, so
    // no false zero is emitted.
    std::string power_acquisition = "disabled";
    std::string power_source = "unknown";
    std::uint64_t power_sample_id = 0u;
    double power_time_ms = std::numeric_limits<double>::quiet_NaN();
    double power_mw = std::numeric_limits<double>::quiet_NaN();
};

struct RuntimeSnapshot {
    std::string snapshot_time_iso;
    bool policy_writes_enabled_present = false;
    bool policy_writes_enabled = false;
    std::vector<RuntimeAmdSensor> amd_sensors;
    RuntimeGpuSnapshot gpu;
    std::vector<RuntimeFanSnapshot> fans;

    // FEAT-0006 read-only RAPL package-energy evidence (REQ-CPUEFF-02), copied
    // from AmdSnapshot. Raw and quarantined; average watts is derived later by
    // the analyzer. acquisition: "disabled" | "unavailable" | "quarantine" |
    // "validated". sample id increments only on a new ~1 s window (de-dup key);
    // window/delta are blank (NaN) when unavailable or guard-blanked. Carried
    // in-memory to the control-loop CSV; not part of the snapshot JSON sidecar.
    std::string pkg_energy_acquisition = "disabled";
    std::uint64_t pkg_energy_sample_id = 0u;
    double pkg_energy_window_ms = std::numeric_limits<double>::quiet_NaN();
    double pkg_energy_delta_uj = std::numeric_limits<double>::quiet_NaN();

    // FEAT-0006 read-only APERF/MPERF cycle evidence (REQ-CPUEFF-01/03), copied
    // from AmdSnapshot. Raw delivered (dAPERF) / reference (dMPERF) cycles per
    // ~1 s window; the analyzer derives effective frequency. Same provenance
    // states; deltas blank (NaN) on baseline / unavailable / guard-blank.
    std::string cpu_cycles_acquisition = "disabled";
    std::uint64_t cpu_cycles_sample_id = 0u;
    double cpu_cycles_window_ms = std::numeric_limits<double>::quiet_NaN();
    double cpu_aperf_delta = std::numeric_limits<double>::quiet_NaN();
    double cpu_mperf_delta = std::numeric_limits<double>::quiet_NaN();
};

// Per-snapshot lookup cache for hot paths that need to query the same sampled
// telemetry repeatedly. Rebuild it after the source RuntimeSnapshot changes.
// Duplicate labels/channels preserve the existing first-match semantics of the
// linear FindRuntime* helpers.
class RuntimeSnapshotIndex {
public:
    void Rebuild(const RuntimeSnapshot& snapshot);
    void Clear();

    const RuntimeFanSnapshot* FindFanChannel(std::uint32_t channel) const;
    const RuntimeAmdSensor* FindAmdSensor(std::string_view label) const;
    double FindAmdSensorTemperature(std::string_view label) const;

private:
    std::unordered_map<std::uint32_t, const RuntimeFanSnapshot*> fans_by_channel_;
    std::unordered_map<std::string_view, const RuntimeAmdSensor*>
        amd_sensors_by_label_;
};

// Parses the runtime snapshot JSON into a product-owned in-memory model that
// the rest of Control can consume without depending on raw JSON structure
// scanning.
RuntimeSnapshot ParseRuntimeSnapshotJson(const std::string& snapshot_json);

const RuntimeFanSnapshot* FindRuntimeFanChannel(const RuntimeSnapshot& snapshot,
                                                std::uint32_t channel);
RuntimeFanSnapshot* FindMutableRuntimeFanChannel(RuntimeSnapshot& snapshot,
                                                 std::uint32_t channel);
RuntimeFanSnapshot& UpsertRuntimeFanChannel(RuntimeSnapshot& snapshot,
                                            std::uint32_t channel);

double FindRuntimeAmdSensorTemperature(const RuntimeSnapshot& snapshot,
                                       const std::string& label);

std::string SerializeRuntimeSnapshotJson(const RuntimeSnapshot& snapshot);
bool WriteRuntimeSnapshotJsonFile(const std::filesystem::path& target_path,
                                  const RuntimeSnapshot& snapshot);
bool WriteRuntimeSnapshotFile(const std::filesystem::path& runtime_home,
                              const RuntimeSnapshot& snapshot);

}  // namespace svg_mb_control
