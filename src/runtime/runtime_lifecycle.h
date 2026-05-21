#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace svg_mb_control {

struct RuntimeBreakerResetRequest {
    std::optional<std::uint32_t> channel;
    std::string requested_at;
    std::string parse_error;
};

std::filesystem::path RuntimeStopRequestPath(
    const std::filesystem::path& runtime_home);

std::filesystem::path RuntimeBreakerResetRequestPath(
    const std::filesystem::path& runtime_home);

bool RuntimeStopRequested(const std::filesystem::path& runtime_home);

bool RequestRuntimeStop(const std::filesystem::path& runtime_home);

bool RequestRuntimeBreakerReset(
    const std::filesystem::path& runtime_home,
    std::optional<std::uint32_t> channel = std::nullopt);

std::optional<RuntimeBreakerResetRequest> TakeRuntimeBreakerResetRequest(
    const std::filesystem::path& runtime_home);

void ClearRuntimeStopRequest(const std::filesystem::path& runtime_home);

void ClearRuntimeBreakerResetRequest(
    const std::filesystem::path& runtime_home);

}  // namespace svg_mb_control
