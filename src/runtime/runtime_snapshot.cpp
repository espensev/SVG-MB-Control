#include "runtime_snapshot.h"

#include "json_io.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace svg_mb_control {

namespace {

RuntimeAmdSensor AmdSensorFromJson(const nlohmann::json& item) {
    RuntimeAmdSensor sensor;
    sensor.label = item.value("label", std::string());
    sensor.temperature_c = item.value("temperature_c", 0.0);
    return sensor;
}

RuntimeFanSnapshot FanSnapshotFromJson(const nlohmann::json& item) {
    RuntimeFanSnapshot fan;
    fan.channel = item.value("channel", 0u);
    fan.label = item.value("label", std::string());
    fan.rpm = static_cast<std::uint16_t>(item.value("rpm", 0u));
    fan.tach_raw = static_cast<std::uint16_t>(item.value("tach_raw", 0u));
    fan.duty_raw = static_cast<std::uint8_t>(item.value("duty_raw", 0u));
    fan.mode_raw = static_cast<std::uint8_t>(item.value("mode_raw", 0u));
    fan.duty_percent = item.value("duty_percent", 0.0);
    fan.tach_valid = item.value("tach_valid", false);
    fan.manual_override = item.value("manual_override", false);
    fan.write_allowed = item.value("write_allowed", false);
    fan.policy_blocked = item.value("policy_blocked", false);
    fan.effective_write_allowed =
        item.value("effective_write_allowed", false);
    return fan;
}

nlohmann::json RuntimeSnapshotToJson(const RuntimeSnapshot& snapshot) {
    nlohmann::json payload = MakeSchemaObject(1u);
    payload["snapshot_time"] = snapshot.snapshot_time_iso;
    payload["policy_writes_enabled_present"] =
        snapshot.policy_writes_enabled_present;
    payload["policy_writes_enabled"] = snapshot.policy_writes_enabled;

    payload["amd_sensors"] = nlohmann::json::array();
    for (const auto& sensor : snapshot.amd_sensors) {
        payload["amd_sensors"].push_back({
            {"label", sensor.label},
            {"temperature_c", sensor.temperature_c},
        });
    }

    payload["gpu"] = {
        {"available", snapshot.gpu.available},
        {"core_c", snapshot.gpu.core_c},
        {"memjn_c", snapshot.gpu.memjn_c},
        {"hotspot_c", snapshot.gpu.hotspot_c},
        {"gpu_name", snapshot.gpu.gpu_name},
        {"last_warning", snapshot.gpu.last_warning},
    };

    payload["fans"] = nlohmann::json::array();
    for (const auto& fan : snapshot.fans) {
        payload["fans"].push_back({
            {"channel", fan.channel},
            {"label", fan.label},
            {"rpm", fan.rpm},
            {"tach_raw", fan.tach_raw},
            {"duty_raw", static_cast<unsigned int>(fan.duty_raw)},
            {"mode_raw", static_cast<unsigned int>(fan.mode_raw)},
            {"duty_percent", fan.duty_percent},
            {"tach_valid", fan.tach_valid},
            {"manual_override", fan.manual_override},
            {"write_allowed", fan.write_allowed},
            {"policy_blocked", fan.policy_blocked},
            {"effective_write_allowed", fan.effective_write_allowed},
        });
    }

    return payload;
}

}  // namespace

RuntimeSnapshot ParseRuntimeSnapshotJson(const std::string& snapshot_json) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(snapshot_json);
    } catch (const nlohmann::json::parse_error& error) {
        throw std::runtime_error(std::string("runtime snapshot JSON parse error: ") +
                                 error.what());
    }

    RuntimeSnapshot snapshot;
    snapshot.snapshot_time_iso = root.value("snapshot_time", std::string());
    if (root.contains("policy_writes_enabled_present")) {
        snapshot.policy_writes_enabled_present =
            root.value("policy_writes_enabled_present", false);
    } else if (root.contains("policy_writes_enabled")) {
        snapshot.policy_writes_enabled_present = true;
    }
    snapshot.policy_writes_enabled =
        root.value("policy_writes_enabled", false);

    if (const auto amd = root.find("amd_sensors");
        amd != root.end() && amd->is_array()) {
        snapshot.amd_sensors.reserve(amd->size());
        for (const auto& item : *amd) {
            if (item.is_object()) {
                snapshot.amd_sensors.push_back(AmdSensorFromJson(item));
            }
        }
    }

    if (const auto gpu = root.find("gpu"); gpu != root.end() && gpu->is_object()) {
        snapshot.gpu.available = gpu->value("available", false);
        snapshot.gpu.core_c = gpu->value("core_c", 0.0);
        snapshot.gpu.memjn_c = gpu->value("memjn_c", 0.0);
        snapshot.gpu.hotspot_c = gpu->value("hotspot_c", 0.0);
        snapshot.gpu.gpu_name = gpu->value("gpu_name", std::string());
        snapshot.gpu.last_warning = gpu->value("last_warning", std::string());
    }

    if (const auto fans = root.find("fans");
        fans != root.end() && fans->is_array()) {
        snapshot.fans.reserve(fans->size());
        for (const auto& item : *fans) {
            if (item.is_object()) {
                snapshot.fans.push_back(FanSnapshotFromJson(item));
            }
        }
    }

    return snapshot;
}

const RuntimeFanSnapshot* FindRuntimeFanChannel(const RuntimeSnapshot& snapshot,
                                                std::uint32_t channel) {
    for (const auto& fan : snapshot.fans) {
        if (fan.channel == channel) {
            return &fan;
        }
    }
    return nullptr;
}

RuntimeFanSnapshot* FindMutableRuntimeFanChannel(RuntimeSnapshot& snapshot,
                                                 std::uint32_t channel) {
    for (auto& fan : snapshot.fans) {
        if (fan.channel == channel) {
            return &fan;
        }
    }
    return nullptr;
}

RuntimeFanSnapshot& UpsertRuntimeFanChannel(RuntimeSnapshot& snapshot,
                                            std::uint32_t channel) {
    if (RuntimeFanSnapshot* existing =
            FindMutableRuntimeFanChannel(snapshot, channel)) {
        return *existing;
    }
    snapshot.fans.push_back(RuntimeFanSnapshot{});
    snapshot.fans.back().channel = channel;
    return snapshot.fans.back();
}

double FindRuntimeAmdSensorTemperature(const RuntimeSnapshot& snapshot,
                                       const std::string& label) {
    for (const auto& sensor : snapshot.amd_sensors) {
        if (sensor.label == label) {
            return sensor.temperature_c;
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

std::string SerializeRuntimeSnapshotJson(const RuntimeSnapshot& snapshot) {
    return RuntimeSnapshotToJson(snapshot).dump(2) + "\n";
}

bool WriteRuntimeSnapshotJsonFile(const std::filesystem::path& target_path,
                                  const RuntimeSnapshot& snapshot) {
    if (target_path.empty()) {
        return false;
    }
    return TryWriteJsonFileAtomic(target_path, RuntimeSnapshotToJson(snapshot));
}

bool WriteRuntimeSnapshotFile(const std::filesystem::path& runtime_home,
                              const RuntimeSnapshot& snapshot) {
    return WriteRuntimeSnapshotJsonFile(runtime_home / "current_state.json",
                                        snapshot);
}

}  // namespace svg_mb_control
