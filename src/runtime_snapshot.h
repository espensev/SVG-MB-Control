#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
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
};

struct RuntimeSnapshot {
    std::string snapshot_time_iso;
    bool policy_writes_enabled_present = false;
    bool policy_writes_enabled = false;
    std::vector<RuntimeAmdSensor> amd_sensors;
    RuntimeGpuSnapshot gpu;
    std::vector<RuntimeFanSnapshot> fans;
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
