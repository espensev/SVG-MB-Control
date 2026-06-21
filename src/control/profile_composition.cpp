#include "profile_composition.h"

#include "json_io.h"

#include <map>
#include <stdexcept>
#include <string>

namespace svg_mb_control {

namespace {

std::filesystem::path ResolveRelativeTo(const std::filesystem::path& base_dir,
                                        const std::string& raw) {
    std::filesystem::path path(raw);
    if (path.is_relative()) {
        path = base_dir / path;
    }
    return std::filesystem::absolute(path).lexically_normal();
}

// Builds channel -> minimum-duty for every controlled channel in a
// machine_cooling_policy.v1 file. A fan is controlled when it has a numeric
// `release_min_duty_pct` and is not an excluded (AIO/pump) channel.
std::map<std::uint32_t, double> ControlledChannelMinDuty(
    const nlohmann::json& policy,
    const std::filesystem::path& policy_path) {
    if (!policy.contains("fans") || !policy["fans"].is_array()) {
        throw std::runtime_error(
            "machine base policy missing fans array: " + policy_path.string());
    }
    std::map<std::uint32_t, double> min_duty;
    for (const auto& fan : policy["fans"]) {
        if (!fan.is_object() || !fan.contains("channel")) {
            continue;
        }
        const std::string direction = fan.value("direction", std::string());
        if (direction == "excluded") {
            continue;
        }
        const auto md = fan.find("release_min_duty_pct");
        if (md == fan.end() || !md->is_number()) {
            continue;
        }
        min_duty[fan["channel"].get<std::uint32_t>()] = md->get<double>();
    }
    return min_duty;
}

}  // namespace

ComposedConfig ComposeConfigRoot(const std::filesystem::path& config_path) {
    const std::filesystem::path absolute =
        std::filesystem::absolute(config_path).lexically_normal();
    nlohmann::json root = ReadJsonFile(absolute, "control config");

    if (!root.is_object() || !root.contains("compose")) {
        return ComposedConfig{std::move(root), absolute};
    }

    const auto& compose = root["compose"];
    if (!compose.is_object() || !compose.contains("machine_base") ||
        !compose.contains("behavior_overlay")) {
        throw std::runtime_error(
            "compose requires string machine_base and behavior_overlay: " +
            absolute.string());
    }

    const std::filesystem::path base_dir = absolute.parent_path();
    const std::filesystem::path policy_path = ResolveRelativeTo(
        base_dir, compose["machine_base"].get<std::string>());
    const std::filesystem::path overlay_path = ResolveRelativeTo(
        base_dir, compose["behavior_overlay"].get<std::string>());

    const nlohmann::json policy =
        ReadJsonFile(policy_path, "machine base policy");
    nlohmann::json overlay = ReadJsonFile(overlay_path, "behavior overlay");

    const std::map<std::uint32_t, double> min_duty =
        ControlledChannelMinDuty(policy, policy_path);

    if (!overlay.is_object() || !overlay.contains("control_loop") ||
        !overlay["control_loop"].is_object() ||
        !overlay["control_loop"].contains("channels") ||
        !overlay["control_loop"]["channels"].is_array()) {
        throw std::runtime_error(
            "behavior overlay missing control_loop.channels: " +
            overlay_path.string());
    }

    // Inject the machine-base minimum duty into each overlay channel. The
    // overlay owns the channel order and behavior; the machine base owns the
    // physical floor and the controlled-channel set.
    for (auto& channel : overlay["control_loop"]["channels"]) {
        if (!channel.is_object() || !channel.contains("channel")) {
            continue;
        }
        const std::uint32_t number = channel["channel"].get<std::uint32_t>();
        const auto found = min_duty.find(number);
        if (found == min_duty.end()) {
            throw std::runtime_error(
                "behavior overlay channel " + std::to_string(number) +
                " is not a controlled channel in the machine base: " +
                policy_path.string());
        }
        channel["min_duty_pct"] = found->second;
    }

    // base_path is the overlay path: the overlay declares the runtime/snapshot/
    // policy path fields, so the merged config resolves them relative to the
    // overlay's own directory (the layer that owns those fields).
    return ComposedConfig{std::move(overlay), overlay_path};
}

}  // namespace svg_mb_control
