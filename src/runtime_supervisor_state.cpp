#include "runtime_supervisor_state.h"

#include "json_io.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace svg_mb_control {

namespace {

std::string JsonStringOr(const nlohmann::json& value,
                         std::string_view key,
                         std::string_view fallback = {}) {
    const auto found = value.find(std::string(key));
    if (found != value.end() && found->is_string()) {
        return found->get<std::string>();
    }
    return std::string(fallback);
}

std::uint32_t JsonUInt32Or(const nlohmann::json& value,
                           std::string_view key,
                           std::uint32_t fallback = 0u) {
    const auto found = value.find(std::string(key));
    if (found != value.end() && found->is_number_unsigned()) {
        return found->get<std::uint32_t>();
    }
    if (found != value.end() && found->is_number_integer()) {
        const auto raw = found->get<std::int64_t>();
        if (raw >= 0 && raw <= static_cast<std::int64_t>(UINT32_MAX)) {
            return static_cast<std::uint32_t>(raw);
        }
    }
    return fallback;
}

}  // namespace

std::filesystem::path RuntimeSupervisorStatePath(
    const std::filesystem::path& runtime_home) {
    return runtime_home / "control_supervisor.json";
}

nlohmann::json SupervisorStateToJson(const SupervisorState& state) {
    nlohmann::json payload = MakeSchemaObject(1u);
    payload["supervisor_pid"] = state.supervisor_pid;
    payload["worker_restart_count"] = state.worker_restart_count;
    payload["last_worker_pid"] = state.last_worker_pid;
    payload["last_worker_started_time"] = state.last_worker_started_time;
    payload["last_worker_restart_time"] = state.last_worker_restart_time;
    payload["last_worker_exit_time"] = state.last_worker_exit_time;
    payload["last_worker_exit_code"] =
        state.has_last_worker_exit_code
            ? nlohmann::json(state.last_worker_exit_code)
            : nlohmann::json(nullptr);
    return payload;
}

bool WriteSupervisorState(const std::filesystem::path& runtime_home,
                          const SupervisorState& state) {
    return TryWriteJsonFileAtomic(RuntimeSupervisorStatePath(runtime_home),
                                  SupervisorStateToJson(state));
}

std::optional<SupervisorState> ReadSupervisorState(
    const std::filesystem::path& runtime_home) {
    nlohmann::json payload;
    try {
        payload = ReadJsonFile(RuntimeSupervisorStatePath(runtime_home),
                               "supervisor state");
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!payload.is_object()) {
        return std::nullopt;
    }

    SupervisorState state;
    state.supervisor_pid = JsonUInt32Or(payload, "supervisor_pid");
    state.worker_restart_count = JsonUInt32Or(payload, "worker_restart_count");
    state.last_worker_pid = JsonUInt32Or(payload, "last_worker_pid");
    state.last_worker_started_time =
        JsonStringOr(payload, "last_worker_started_time");
    state.last_worker_restart_time =
        JsonStringOr(payload, "last_worker_restart_time");
    state.last_worker_exit_time =
        JsonStringOr(payload, "last_worker_exit_time");
    const auto exit_code = payload.find("last_worker_exit_code");
    if (exit_code != payload.end() && exit_code->is_number_integer()) {
        state.has_last_worker_exit_code = true;
        state.last_worker_exit_code = exit_code->get<std::int64_t>();
    }
    return state;
}

}  // namespace svg_mb_control
