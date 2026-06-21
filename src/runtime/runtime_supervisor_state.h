#pragma once

// Supervisor-owned runtime sidecar. RunSupervisedLongRunningMode is the only
// process that knows worker restart counts and worker exit codes; the worker
// owns control_runtime.json and rewrites it atomically every tick, so the
// supervisor publishes its state into a separate control_supervisor.json
// sidecar instead. --health / --status / the eval dashboard merge this with
// the worker status, matching the existing sidecar pattern used for the
// stop-request and pending-writes files.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace svg_mb_control {

struct SupervisorState {
    std::uint32_t supervisor_pid = 0u;
    std::uint32_t worker_restart_count = 0u;
    std::uint32_t last_worker_pid = 0u;
    std::string last_worker_started_time;   // local ISO8601, empty until first start
    std::string last_worker_restart_time;   // local ISO8601, empty until first restart
    std::string last_worker_exit_time;      // local ISO8601, empty until first exit
    bool has_last_worker_exit_code = false;
    std::int64_t last_worker_exit_code = 0;
    std::string active_profile_name;  // FEAT-0023: active profile the worker runs
};

std::filesystem::path RuntimeSupervisorStatePath(
    const std::filesystem::path& runtime_home);

nlohmann::json SupervisorStateToJson(const SupervisorState& state);

// Best-effort atomic write of control_supervisor.json. Returns false on
// failure; the supervisor treats this as non-fatal.
bool WriteSupervisorState(const std::filesystem::path& runtime_home,
                          const SupervisorState& state);

// Tolerant read. Returns std::nullopt when the file is absent or unreadable;
// individual missing fields fall back to their defaults.
std::optional<SupervisorState> ReadSupervisorState(
    const std::filesystem::path& runtime_home);

}  // namespace svg_mb_control
