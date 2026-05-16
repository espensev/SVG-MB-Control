#include "control_config.h"

#include "json_io.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <stdexcept>
#include <vector>

namespace svg_mb_control {

namespace {

bool ContainsKeyRecursive(const nlohmann::json& value, std::string_view key) {
    if (value.is_object()) {
        if (value.contains(std::string(key))) {
            return true;
        }
        for (const auto& item : value.items()) {
            if (ContainsKeyRecursive(item.value(), key)) {
                return true;
            }
        }
        return false;
    }
    if (value.is_array()) {
        for (const auto& item : value) {
            if (ContainsKeyRecursive(item, key)) {
                return true;
            }
        }
    }
    return false;
}

std::filesystem::path ResolveConfigRelativePath(
    const std::filesystem::path& config_path,
    const std::string& raw_value) {
    std::filesystem::path path(raw_value);
    if (path.empty()) {
        return {};
    }
    if (path.is_relative()) {
        path = config_path.parent_path() / path;
    }
    return std::filesystem::absolute(path).lexically_normal();
}

void RejectLegacyFieldIfPresent(const nlohmann::json& root,
                                std::string_view key,
                                std::string_view guidance) {
    if (!ContainsKeyRecursive(root, key)) {
        return;
    }

    std::string message = "Legacy control config field was removed: ";
    message += key;
    if (!guidance.empty()) {
        message += ". ";
        message += guidance;
    }
    throw std::runtime_error(message);
}

}  // namespace

std::filesystem::path GetEnvironmentPath(std::wstring_view name) {
    std::array<wchar_t, 4096> buffer{};
    const DWORD length = GetEnvironmentVariableW(std::wstring(name).c_str(),
                                                 buffer.data(),
                                                 static_cast<DWORD>(buffer.size()));
    if (length == 0u || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(buffer.data(), buffer.data() + length);
}

std::filesystem::path ResolveDefaultControlConfigPath() {
    const std::filesystem::path current_exe_dir = []() {
        std::array<wchar_t, MAX_PATH> buffer{};
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size()) {
            return std::filesystem::path();
        }
        return std::filesystem::path(buffer.data(), buffer.data() + length).parent_path();
    }();

    const std::filesystem::path exe_parent =
        current_exe_dir.empty() ? std::filesystem::path{}
                                : current_exe_dir.parent_path();
    const std::filesystem::path exe_grandparent =
        exe_parent.empty() ? std::filesystem::path{}
                           : exe_parent.parent_path();
    const std::vector<std::filesystem::path> candidates = {
        current_exe_dir / "control.json",
        current_exe_dir / "config" / "control.json",
        exe_parent / "control.json",
        exe_parent / "config" / "control.json",
        exe_grandparent / "control.json",
        exe_grandparent / "config" / "control.json",
        std::filesystem::current_path() / "control.json",
        std::filesystem::current_path() / "config" / "control.json",
    };

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!candidate.empty() &&
            std::filesystem::exists(candidate, ec) &&
            !std::filesystem::is_directory(candidate, ec)) {
            return std::filesystem::absolute(candidate).lexically_normal();
        }
    }

    return {};
}

ControlConfig LoadControlConfig(const std::filesystem::path& path) {
    const std::filesystem::path absolute_path =
        std::filesystem::absolute(path).lexically_normal();
    if (!std::filesystem::exists(absolute_path)) {
        throw std::runtime_error("Control config not found: " +
                                 absolute_path.string());
    }

    const nlohmann::json root = ReadJsonFile(absolute_path, "control config");
    ControlConfig config;
    config.source_path = absolute_path;

    RejectLegacyFieldIfPresent(
        root, "bridge_exe_path",
        "This branch runs direct-only. Remove the field from the config.");
    RejectLegacyFieldIfPresent(
        root, "bench_exe_path",
        "This branch runs direct-only. Remove the field from the config.");
    RejectLegacyFieldIfPresent(
        root, "bridge_command",
        "This branch runs direct-only. Remove the field from the config.");
    RejectLegacyFieldIfPresent(
        root, "bench_runtime_policy_path",
        "Use runtime_policy_path instead.");
    RejectLegacyFieldIfPresent(
        root, "logger_service_duration_ms",
        "This branch runs direct-only. Remove the field from the config.");
    RejectLegacyFieldIfPresent(
        root, "snapshot_read_retry_count",
        "This branch runs direct-only. Remove the field from the config.");
    RejectLegacyFieldIfPresent(
        root, "snapshot_read_retry_backoff_ms",
        "This branch runs direct-only. Remove the field from the config.");
    RejectLegacyFieldIfPresent(
        root, "child_restart_budget",
        "This branch runs direct-only. Remove the field from the config.");
    RejectLegacyFieldIfPresent(
        root, "child_restart_backoff_ms",
        "This branch runs direct-only. Remove the field from the config.");

    config.schema_version = root.value("schema_version", config.schema_version);
    config.default_mode = root.value("default_mode", config.default_mode);
    config.poll_ms = root.value("poll_ms", config.poll_ms);
    config.staleness_threshold_ms =
        root.value("staleness_threshold_ms", config.staleness_threshold_ms);
    config.log_rotate_hours =
        root.value("log_rotate_hours", config.log_rotate_hours);
    config.log_retain_days =
        root.value("log_retain_days", config.log_retain_days);
    config.evidence_gpu_sample_mode = root.value(
        "evidence_gpu_sample_mode", config.evidence_gpu_sample_mode);
    config.baseline_freshness_ceiling_ms = root.value(
        "baseline_freshness_ceiling_ms",
        config.baseline_freshness_ceiling_ms);
    config.restore_timeout_ms =
        root.value("restore_timeout_ms", config.restore_timeout_ms);

    if (const auto snapshot_path = root.find("snapshot_path");
        snapshot_path != root.end()) {
        config.snapshot_path = ResolveConfigRelativePath(
            absolute_path, snapshot_path->get<std::string>());
    }
    if (const auto runtime_home = root.find("runtime_home_path");
        runtime_home != root.end()) {
        config.runtime_home_path = ResolveConfigRelativePath(
            absolute_path, runtime_home->get<std::string>());
    }
    if (const auto policy = root.find("runtime_policy_path");
        policy != root.end()) {
        config.runtime_policy_path = ResolveConfigRelativePath(
            absolute_path, policy->get<std::string>());
    }

    if (const auto write_channel = root.find("write_channel");
        write_channel != root.end()) {
        config.write_channel = write_channel->get<std::uint32_t>();
        config.write_channel_set = true;
    }
    if (const auto write_pct = root.find("write_target_pct");
        write_pct != root.end()) {
        config.write_target_pct = write_pct->get<double>();
        config.write_target_pct_set = true;
    }
    if (const auto hold_ms = root.find("write_hold_ms");
        hold_ms != root.end()) {
        config.write_hold_ms = hold_ms->get<std::uint32_t>();
        config.write_hold_ms_set = true;
    }

    return config;
}

}  // namespace svg_mb_control
