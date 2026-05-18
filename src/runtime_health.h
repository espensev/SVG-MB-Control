#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace svg_mb_control {

enum class RuntimeHealthState {
    kHealthy,
    kDegraded,
    kStale,
    kStopped,
    kFailed,
};

struct RuntimeHealthResult {
    RuntimeHealthState state = RuntimeHealthState::kStopped;
    std::string reason;
    std::filesystem::path runtime_home;
    std::filesystem::path status_path;
    bool status_file_present = false;
    bool status_json_valid = false;
    bool process_active = false;
    std::uint32_t process_id = 0u;
    std::string mode;
    std::string status;
    std::string status_detail;
    std::string last_update;
    std::int64_t last_update_age_ms = -1;
    std::uint32_t stale_after_ms = 10000u;
    bool stop_request_present = false;
    std::uint32_t pending_write_count = 0u;
    bool pending_writes_unreadable = false;
    std::uint32_t degraded_channel_count = 0u;
    std::string last_successful_restore_time;

    // Merged from the supervisor-owned control_supervisor.json sidecar.
    bool supervisor_state_present = false;
    std::uint32_t supervisor_pid = 0u;
    bool supervisor_active = false;
    std::uint32_t worker_restart_count = 0u;
    std::uint32_t last_worker_pid = 0u;
    std::string last_worker_started_time;
    std::string last_worker_restart_time;
    std::string last_worker_exit_time;
    bool has_last_worker_exit_code = false;
    std::int64_t last_worker_exit_code = 0;
};

const char* RuntimeHealthStateName(RuntimeHealthState state);
int RuntimeHealthExitCode(RuntimeHealthState state);

RuntimeHealthResult EvaluateRuntimeHealth(
    const std::filesystem::path& runtime_home,
    std::uint32_t stale_after_ms = 10000u);

nlohmann::json RuntimeHealthToJson(const RuntimeHealthResult& result);

int PrintRuntimeHealth(const std::filesystem::path& runtime_home,
                       bool json_output,
                       std::uint32_t stale_after_ms = 10000u);

}  // namespace svg_mb_control
