#include "runtime_health.h"

#include "control_scheduler.h"
#include "json_io.h"
#include "pending_writes.h"
#include "runtime_lifecycle.h"
#include "runtime_paths.h"
#include "runtime_status.h"
#include "runtime_supervisor_state.h"
#include "runtime_util.h"

#include "windows_lean.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>

namespace svg_mb_control {

namespace {

void FillSidecarHealth(RuntimeHealthResult* result) {
    if (result == nullptr) {
        return;
    }

    result->stop_request_present = RuntimeStopRequested(result->runtime_home);

    try {
        const std::vector<PendingWriteEntry> entries =
            ReadPendingWrites(result->runtime_home);
        result->pending_write_count =
            static_cast<std::uint32_t>((std::min)(
                entries.size(),
                static_cast<std::size_t>(UINT32_MAX)));
    } catch (const std::exception&) {
        // Health probe surfaces the unreadable state via the boolean flag;
        // exception text is intentionally dropped because operators inspect
        // pending_writes.json directly when this flag is set.
        result->pending_writes_unreadable = true;
    }

    const auto supervisor = ReadSupervisorState(result->runtime_home);
    if (supervisor.has_value()) {
        result->supervisor_state_present = true;
        result->supervisor_pid = supervisor->supervisor_pid;
        result->supervisor_active = IsProcessActive(supervisor->supervisor_pid);
        result->worker_restart_count = supervisor->worker_restart_count;
        result->last_worker_pid = supervisor->last_worker_pid;
        result->last_worker_started_time = supervisor->last_worker_started_time;
        result->last_worker_restart_time = supervisor->last_worker_restart_time;
        result->last_worker_exit_time = supervisor->last_worker_exit_time;
        result->has_last_worker_exit_code =
            supervisor->has_last_worker_exit_code;
        result->last_worker_exit_code = supervisor->last_worker_exit_code;
    }
}

void SetState(RuntimeHealthResult* result,
              RuntimeHealthState state,
              std::string reason) {
    result->state = state;
    result->reason = std::move(reason);
}

// Runs the cascading health checks against an already-initialized result.
// Each early return stops the assessment with the last SetState() applied;
// the outer EvaluateRuntimeHealth is responsible for setup and return value.
void AssessHealthState(RuntimeHealthResult& result,
                       const std::filesystem::path& runtime_home) {
    std::error_code ec;
    result.status_file_present =
        std::filesystem::exists(result.status_path, ec);
    if (ec || !result.status_file_present) {
        SetState(&result, RuntimeHealthState::kStopped,
                 "runtime status file is not present");
        return;
    }

    const auto snapshot = ReadRuntimeStatus(runtime_home);
    if (!snapshot.has_value()) {
        result.status_json_valid = false;
        SetState(&result, RuntimeHealthState::kFailed,
                 "runtime status JSON is not readable or not an object");
        return;
    }
    result.status_json_valid = true;

    result.mode = snapshot->mode.empty() ? "(unknown)" : snapshot->mode;
    result.status = snapshot->status.empty() ? "(unknown)" : snapshot->status;
    result.status_detail = snapshot->status_detail;
    result.last_update = snapshot->last_update_iso;
    result.process_id = snapshot->process_id;
    result.process_active = IsProcessActive(result.process_id);
    result.degraded_channel_count = snapshot->DegradedChannelCount();
    result.last_successful_restore_time =
        snapshot->last_successful_restore_iso;

    if (result.pending_writes_unreadable) {
        SetState(&result, RuntimeHealthState::kFailed,
                 "pending-writes sidecar is unreadable");
        return;
    }

    if (result.status == "failed" || result.status == "direct-read-failed") {
        const std::string suffix =
            result.status_detail.empty() ? "" : ": " + result.status_detail;
        SetState(&result, RuntimeHealthState::kFailed,
                 "runtime status is " + result.status + suffix);
        return;
    }

    if (result.status == "shutdown") {
        SetState(&result, RuntimeHealthState::kStopped,
                 "runtime reported shutdown");
        return;
    }

    if (!result.process_active) {
        SetState(&result, RuntimeHealthState::kStopped,
                 "runtime process is not active");
        return;
    }

    if (snapshot->stale) {
        SetState(&result, RuntimeHealthState::kStale,
                 "runtime status reports stale telemetry");
        return;
    }

    const auto parsed_last_update = ParseLocalIso8601(result.last_update);
    if (!parsed_last_update.has_value()) {
        SetState(&result, RuntimeHealthState::kStale,
                 "runtime status has no parseable last update timestamp");
        return;
    }

    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now() - *parsed_last_update);
    result.last_update_age_ms = age.count();
    if (age.count() < 0) {
        result.last_update_age_ms = 0;
    }
    if (result.last_update_age_ms >
        static_cast<std::int64_t>(result.stale_after_ms)) {
        SetState(&result, RuntimeHealthState::kStale,
                 "runtime status is older than the health staleness threshold");
        return;
    }

    if (result.stop_request_present) {
        SetState(&result, RuntimeHealthState::kDegraded,
                 "stop request is present while runtime process is active");
        return;
    }

    if (result.degraded_channel_count > 0u) {
        SetState(&result, RuntimeHealthState::kDegraded,
                 "one or more controlled channels report degraded state");
        return;
    }

    if (result.status != "running") {
        SetState(&result, RuntimeHealthState::kDegraded,
                 "runtime status is not running");
        return;
    }

    SetState(&result, RuntimeHealthState::kHealthy,
             "runtime process is active and status is fresh");
}

// Best-effort persistence of the last health assessment so the watchdog,
// status command, and eval dashboard can show the most recent result
// without re-evaluating. Written only by the --health CLI path; pure
// evaluation stays side-effect free.
void PersistHealthAssessment(const RuntimeHealthResult& result) {
    nlohmann::json payload = MakeSchemaObject(1u);
    payload["last_health_state"] = RuntimeHealthStateName(result.state);
    payload["last_health_reason"] = result.reason;
    payload["last_health_exit_code"] = RuntimeHealthExitCode(result.state);
    payload["last_health_time"] =
        FormatLocalIso8601(std::chrono::system_clock::now());
    if (!TryWriteJsonFileAtomic(result.runtime_home / "control_health.json",
                                payload)) {
        std::cerr << "warning: failed to write control_health.json to "
                  << result.runtime_home.string() << '\n';
    }
}

}  // namespace

const char* RuntimeHealthStateName(RuntimeHealthState state) {
    switch (state) {
        case RuntimeHealthState::kHealthy:
            return "healthy";
        case RuntimeHealthState::kDegraded:
            return "degraded";
        case RuntimeHealthState::kStale:
            return "stale";
        case RuntimeHealthState::kStopped:
            return "stopped";
        case RuntimeHealthState::kFailed:
            return "failed";
    }
    return "failed";
}

int RuntimeHealthExitCode(RuntimeHealthState state) {
    switch (state) {
        case RuntimeHealthState::kHealthy:
            return 0;
        case RuntimeHealthState::kDegraded:
            return 1;
        case RuntimeHealthState::kStale:
        case RuntimeHealthState::kStopped:
            return 2;
        case RuntimeHealthState::kFailed:
            return 3;
    }
    return 3;
}

RuntimeHealthResult EvaluateRuntimeHealth(
    const std::filesystem::path& runtime_home,
    std::uint32_t stale_after_ms) {
    RuntimeHealthResult result;
    result.runtime_home = runtime_home;
    result.status_path = RuntimeStatusPath(runtime_home);
    result.stale_after_ms = stale_after_ms > 0u ? stale_after_ms : 10000u;
    FillSidecarHealth(&result);
    AssessHealthState(result, runtime_home);
    return result;
}

nlohmann::json RuntimeHealthToJson(const RuntimeHealthResult& result) {
    nlohmann::json payload = MakeSchemaObject(1u);
    payload["health_state"] = RuntimeHealthStateName(result.state);
    payload["reason"] = result.reason;
    payload["exit_code"] = RuntimeHealthExitCode(result.state);
    payload["runtime_home"] = result.runtime_home.string();
    payload["status_file"] = result.status_path.string();
    payload["status_file_present"] = result.status_file_present;
    payload["status_json_valid"] = result.status_json_valid;
    payload["mode"] = result.mode;
    payload["status"] = result.status;
    payload["status_detail"] = result.status_detail;
    payload["process_id"] = result.process_id;
    payload["process_active"] = result.process_active;
    payload["last_update"] = result.last_update;
    payload["last_update_age_ms"] =
        result.last_update_age_ms >= 0 ? nlohmann::json(result.last_update_age_ms)
                                       : nlohmann::json(nullptr);
    payload["stale_after_ms"] = result.stale_after_ms;
    payload["stop_request_present"] = result.stop_request_present;
    payload["pending_write_count"] = result.pending_write_count;
    payload["pending_writes_unreadable"] = result.pending_writes_unreadable;
    payload["degraded_channel_count"] = result.degraded_channel_count;
    payload["last_successful_restore_time"] =
        result.last_successful_restore_time;
    payload["supervisor_state_present"] = result.supervisor_state_present;
    payload["supervisor_pid"] = result.supervisor_pid;
    payload["supervisor_active"] = result.supervisor_active;
    payload["worker_restart_count"] = result.worker_restart_count;
    payload["last_worker_pid"] = result.last_worker_pid;
    payload["last_worker_started_time"] = result.last_worker_started_time;
    payload["last_worker_restart_time"] = result.last_worker_restart_time;
    payload["last_worker_exit_time"] = result.last_worker_exit_time;
    payload["last_worker_exit_code"] =
        result.has_last_worker_exit_code
            ? nlohmann::json(result.last_worker_exit_code)
            : nlohmann::json(nullptr);
    return payload;
}

int PrintRuntimeHealth(const std::filesystem::path& runtime_home,
                       bool json_output,
                       std::uint32_t stale_after_ms) {
    const RuntimeHealthResult result =
        EvaluateRuntimeHealth(runtime_home, stale_after_ms);
    PersistHealthAssessment(result);
    if (json_output) {
        std::cout << RuntimeHealthToJson(result).dump(2) << '\n';
    } else {
        std::cout << "svg-mb-control: "
                  << RuntimeHealthStateName(result.state) << '\n'
                  << "  reason: " << result.reason << '\n'
                  << "  runtime_home: " << result.runtime_home.string()
                  << '\n'
                  << "  status_file: " << result.status_path.string()
                  << '\n';
        if (result.process_id != 0u) {
            std::cout << "  pid: " << result.process_id
                      << (result.process_active ? " (active)"
                                                : " (not active)")
                      << '\n';
        }
        if (!result.mode.empty()) {
            std::cout << "  mode: " << result.mode << '\n';
        }
        if (!result.status.empty()) {
            std::cout << "  status: " << result.status << '\n';
        }
        if (!result.last_update.empty()) {
            std::cout << "  last_update: " << result.last_update << '\n';
        }
        if (!result.last_successful_restore_time.empty()) {
            std::cout << "  last_successful_restore: "
                      << result.last_successful_restore_time << '\n';
        }
        if (result.supervisor_state_present) {
            std::cout << "  supervisor_pid: " << result.supervisor_pid
                      << (result.supervisor_active ? " (active)"
                                                   : " (not active)")
                      << '\n'
                      << "  worker_restart_count: "
                      << result.worker_restart_count << '\n';
            if (result.has_last_worker_exit_code) {
                std::cout << "  last_worker_exit_code: "
                          << result.last_worker_exit_code << '\n';
            }
        }
    }
    return RuntimeHealthExitCode(result.state);
}

int PrintRuntimeStatus(const std::filesystem::path& runtime_home) {
    const std::filesystem::path status_path = RuntimeStatusPath(runtime_home);
    const auto snapshot = ReadRuntimeStatus(runtime_home);
    if (!snapshot.has_value()) {
        std::error_code exists_ec;
        const bool present =
            std::filesystem::exists(status_path, exists_ec) && !exists_ec;
        if (!present) {
            std::cout << "svg-mb-control: not running\n"
                      << "  runtime_home: " << runtime_home.string() << '\n'
                      << "  status: " << status_path.string()
                      << " (not found)\n";
            return 0;
        }
        std::cerr << "svg-mb-control: status file present but unreadable\n"
                  << "  runtime_home: " << runtime_home.string() << '\n'
                  << "  status: " << status_path.string() << '\n';
        return 1;
    }

    const std::string mode =
        snapshot->mode.empty() ? "(unknown)" : snapshot->mode;
    const std::string state =
        snapshot->status.empty() ? "(unknown)" : snapshot->status;
    const bool active = snapshot->LooksActive();

    std::cout << "svg-mb-control: "
              << (active ? "running" : "not running") << '\n'
              << "  mode: " << mode << '\n'
              << "  status: " << state << '\n';
    if (!snapshot->status_detail.empty()) {
        std::cout << "  detail: " << snapshot->status_detail << '\n';
    }
    if (snapshot->process_id != 0u) {
        std::cout << "  pid: " << snapshot->process_id
                  << (active ? " (active)" : " (not active)") << '\n';
    }
    if (!snapshot->last_update_iso.empty()) {
        std::cout << "  last_update: " << snapshot->last_update_iso << '\n';
    }
    std::cout << "  runtime_home: " << runtime_home.string() << '\n'
              << "  status_file: " << status_path.string() << '\n';
    if (!snapshot->log_csv_path.empty()) {
        std::cout << "  csv: " << snapshot->log_csv_path << '\n';
    }
    if (!snapshot->event_log_path.empty()) {
        std::cout << "  events: " << snapshot->event_log_path << '\n';
    }
    if (!snapshot->last_successful_restore_iso.empty()) {
        std::cout << "  last_successful_restore: "
                  << snapshot->last_successful_restore_iso << '\n';
    }

    const auto supervisor = ReadSupervisorState(runtime_home);
    if (supervisor.has_value()) {
        std::cout << "  supervisor_pid: " << supervisor->supervisor_pid
                  << (IsProcessActive(supervisor->supervisor_pid)
                          ? " (active)"
                          : " (not active)")
                  << '\n'
                  << "  worker_restart_count: "
                  << supervisor->worker_restart_count << '\n';
        if (supervisor->has_last_worker_exit_code) {
            std::cout << "  last_worker_exit_code: "
                      << supervisor->last_worker_exit_code;
            if (!supervisor->last_worker_exit_time.empty()) {
                std::cout << " at " << supervisor->last_worker_exit_time;
            }
            std::cout << '\n';
        }
        if (!supervisor->last_worker_restart_time.empty()) {
            std::cout << "  last_worker_restart_time: "
                      << supervisor->last_worker_restart_time << '\n';
        }
    }
    return 0;
}

}  // namespace svg_mb_control
