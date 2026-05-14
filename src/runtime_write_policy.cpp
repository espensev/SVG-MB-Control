#include "runtime_write_policy.h"

#include "json_io.h"

#include <algorithm>
#include <stdexcept>

namespace svg_mb_control {

namespace {

std::filesystem::path ResolveRuntimePolicyPath(const ControlConfig* config) {
    std::filesystem::path path = GetEnvironmentPath(L"SVG_MB_RUNTIME_POLICY");
    if (!path.empty()) {
        return std::filesystem::absolute(path).lexically_normal();
    }
    if (config != nullptr && !config->runtime_policy_path.empty()) {
        return config->runtime_policy_path;
    }
    return {};
}

std::vector<std::uint32_t> ParseBlockedChannels(
    const nlohmann::json& control_json) {
    std::vector<std::uint32_t> blocked;
    const auto channels_it = control_json.find("blocked_channels");
    if (channels_it == control_json.end()) {
        return blocked;
    }
    if (!channels_it->is_array()) {
        throw std::runtime_error(
            "control.blocked_channels must be an array.");
    }

    blocked.reserve(channels_it->size());
    for (const auto& channel : *channels_it) {
        blocked.push_back(channel.get<std::uint32_t>());
    }
    std::sort(blocked.begin(), blocked.end());
    blocked.erase(std::unique(blocked.begin(), blocked.end()), blocked.end());
    return blocked;
}

}  // namespace

RuntimeWritePolicy LoadRuntimeWritePolicy(const std::filesystem::path& path) {
    if (path.empty()) {
        throw std::runtime_error("Runtime policy path is empty.");
    }

    const std::filesystem::path absolute_path =
        std::filesystem::absolute(path).lexically_normal();
    const nlohmann::json root = ReadJsonFile(absolute_path, "runtime policy");

    const auto control_it = root.find("control");
    if (control_it == root.end() || !control_it->is_object()) {
        throw std::runtime_error("Runtime policy missing control object: " +
                                 absolute_path.string());
    }

    RuntimeWritePolicy policy;
    policy.present = true;
    policy.source_path = absolute_path;
    policy.writes_enabled = control_it->value("writes_enabled", false);
    policy.restore_on_exit = control_it->value("restore_on_exit", true);
    policy.blocked_channels = ParseBlockedChannels(*control_it);
    return policy;
}

RuntimeWritePolicy ResolveRuntimeWritePolicy(const ControlConfig* config) {
    const std::filesystem::path path = ResolveRuntimePolicyPath(config);
    if (path.empty()) {
        return RuntimeWritePolicy{};
    }
    return LoadRuntimeWritePolicy(path);
}

bool RuntimeWritePolicyBlocksChannel(const RuntimeWritePolicy& policy,
                                     std::uint32_t channel) {
    return std::find(policy.blocked_channels.begin(),
                     policy.blocked_channels.end(),
                     channel) != policy.blocked_channels.end();
}

}  // namespace svg_mb_control
