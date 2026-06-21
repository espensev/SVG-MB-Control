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

// FEAT-0023: take-once operator request to switch the active profile, consumed
// by the supervisor (not the tick runner) because the switch is delivered by
// restarting the worker. Modeled on the breaker-reset request.
struct RuntimeProfileSwitchRequest {
    std::string profile_name;
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

std::filesystem::path RuntimeProfileSwitchRequestPath(
    const std::filesystem::path& runtime_home);

// Writes a take-once profile-switch request naming the target profile. Returns
// false (and writes nothing) when profile_name is empty.
bool RequestRuntimeProfileSwitch(const std::filesystem::path& runtime_home,
                                 const std::string& profile_name);

// Reads, validates, and clears the profile-switch request in one call. Returns
// std::nullopt when no request file exists. A present-but-malformed request
// returns a value whose parse_error is non-empty.
std::optional<RuntimeProfileSwitchRequest> TakeRuntimeProfileSwitchRequest(
    const std::filesystem::path& runtime_home);

void ClearRuntimeProfileSwitchRequest(
    const std::filesystem::path& runtime_home);

// FEAT-0023 worker-scoped cycle signal: a presence flag (like the stop request,
// and distinct from it) the SUPERVISOR writes to make the running worker break
// its control loop and run its graceful shutdown restore (fans -> BIOS auto) for
// a profile switch. The supervisor is the sole writer and clearer; the worker
// only tests it in its loop condition, alongside RuntimeStopRequested.
std::filesystem::path RuntimeProfileCycleRequestPath(
    const std::filesystem::path& runtime_home);

bool RequestRuntimeProfileCycle(const std::filesystem::path& runtime_home);

bool RuntimeProfileCycleRequested(const std::filesystem::path& runtime_home);

void ClearRuntimeProfileCycleRequest(
    const std::filesystem::path& runtime_home);

}  // namespace svg_mb_control
