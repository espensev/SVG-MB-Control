#include "runtime_supervisor_state.h"

#include "json_io.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace svg_mb_control {

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
    payload["active_profile_name"] = state.active_profile_name;
    return payload;
}

bool WriteSupervisorState(const std::filesystem::path& runtime_home,
                          const SupervisorState& state) {
    return TryWriteJsonFileAtomic(RuntimeSupervisorStatePath(runtime_home),
                                  SupervisorStateToJson(state));
}

std::optional<SupervisorState> ReadSupervisorState(
    const std::filesystem::path& runtime_home) {
    const auto payload = TryReadJsonObject(
        RuntimeSupervisorStatePath(runtime_home), "supervisor state");
    if (!payload.has_value()) {
        return std::nullopt;
    }

    SupervisorState state;
    state.supervisor_pid = JsonUInt32Or(*payload, "supervisor_pid");
    state.worker_restart_count =
        JsonUInt32Or(*payload, "worker_restart_count");
    state.last_worker_pid = JsonUInt32Or(*payload, "last_worker_pid");
    state.last_worker_started_time =
        JsonStringOr(*payload, "last_worker_started_time");
    state.last_worker_restart_time =
        JsonStringOr(*payload, "last_worker_restart_time");
    state.last_worker_exit_time =
        JsonStringOr(*payload, "last_worker_exit_time");
    const auto exit_code = payload->find("last_worker_exit_code");
    if (exit_code != payload->end() && exit_code->is_number_integer()) {
        state.has_last_worker_exit_code = true;
        state.last_worker_exit_code = exit_code->get<std::int64_t>();
    }
    state.active_profile_name = JsonStringOr(*payload, "active_profile_name");
    return state;
}

}  // namespace svg_mb_control
