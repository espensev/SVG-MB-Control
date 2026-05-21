#include "runtime_lifecycle.h"

#include "control_scheduler.h"
#include "json_io.h"

#include <chrono>
#include <ctime>
#include <exception>
#include <limits>
#include <string>
#include <system_error>

namespace svg_mb_control {

std::filesystem::path RuntimeStopRequestPath(
    const std::filesystem::path& runtime_home) {
    return runtime_home / "stop.request.json";
}

std::filesystem::path RuntimeBreakerResetRequestPath(
    const std::filesystem::path& runtime_home) {
    return runtime_home / "circuit_breaker_reset.request.json";
}

bool RuntimeStopRequested(const std::filesystem::path& runtime_home) {
    std::error_code ec;
    return std::filesystem::exists(RuntimeStopRequestPath(runtime_home), ec) &&
           !ec;
}

bool RequestRuntimeStop(const std::filesystem::path& runtime_home) {
    nlohmann::json payload = MakeSchemaObject(1u);
    payload["requested_at"] =
        FormatLocalIso8601(std::chrono::system_clock::now());
    payload["reason"] = "operator-request";
    return TryWriteJsonFileAtomic(RuntimeStopRequestPath(runtime_home),
                                  payload);
}

bool RequestRuntimeBreakerReset(const std::filesystem::path& runtime_home,
                                std::optional<std::uint32_t> channel) {
    nlohmann::json payload = MakeSchemaObject(1u);
    payload["requested_at"] =
        FormatLocalIso8601(std::chrono::system_clock::now());
    payload["reason"] = "operator-request";
    if (channel.has_value()) {
        payload["channel"] = *channel;
    } else {
        payload["channel"] = nullptr;
    }
    return TryWriteJsonFileAtomic(
        RuntimeBreakerResetRequestPath(runtime_home), payload);
}

std::optional<RuntimeBreakerResetRequest> TakeRuntimeBreakerResetRequest(
    const std::filesystem::path& runtime_home) {
    const std::filesystem::path path =
        RuntimeBreakerResetRequestPath(runtime_home);
    std::error_code exists_ec;
    if (!std::filesystem::exists(path, exists_ec) || exists_ec) {
        return std::nullopt;
    }

    RuntimeBreakerResetRequest request;
    try {
        const nlohmann::json payload =
            ReadJsonFile(path, "circuit breaker reset request");
        request.requested_at = JsonStringOr(payload, "requested_at");
        if (auto it = payload.find("channel");
            it != payload.end() && !it->is_null()) {
            if (it->is_number_unsigned()) {
                const auto parsed = it->get<std::uint64_t>();
                if (parsed <=
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::uint32_t>::max())) {
                    request.channel =
                        static_cast<std::uint32_t>(parsed);
                } else {
                    request.parse_error =
                        "channel is outside uint32 range";
                }
            } else if (it->is_number_integer()) {
                const auto parsed = it->get<std::int64_t>();
                if (parsed >= 0 &&
                    parsed <= static_cast<std::int64_t>(
                        std::numeric_limits<std::uint32_t>::max())) {
                    request.channel =
                        static_cast<std::uint32_t>(parsed);
                } else {
                    request.parse_error =
                        "channel is outside uint32 range";
                }
            } else {
                request.parse_error = "channel must be an integer or null";
            }
        }
    } catch (const std::exception& error) {
        request.parse_error = error.what();
    }

    ClearRuntimeBreakerResetRequest(runtime_home);
    return request;
}

void ClearRuntimeStopRequest(const std::filesystem::path& runtime_home) {
    std::error_code ec;
    std::filesystem::remove(RuntimeStopRequestPath(runtime_home), ec);
}

void ClearRuntimeBreakerResetRequest(
    const std::filesystem::path& runtime_home) {
    std::error_code ec;
    std::filesystem::remove(RuntimeBreakerResetRequestPath(runtime_home), ec);
}

}  // namespace svg_mb_control
