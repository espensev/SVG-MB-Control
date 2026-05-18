#include "runtime_health.h"

#include "control_scheduler.h"
#include "json_io.h"
#include "pending_writes.h"
#include "runtime_lifecycle.h"
#include "runtime_supervisor_state.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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
        if (raw > 0 && raw <= static_cast<std::int64_t>(UINT32_MAX)) {
            return static_cast<std::uint32_t>(raw);
        }
    }
    return fallback;
}

bool JsonBoolOr(const nlohmann::json& value,
                std::string_view key,
                bool fallback = false) {
    const auto found = value.find(std::string(key));
    return found != value.end() && found->is_boolean()
               ? found->get<bool>()
               : fallback;
}

std::optional<std::chrono::system_clock::time_point> ParseLocalIso8601(
    const std::string& value) {
    if (value.empty()) {
        return std::nullopt;
    }

    std::tm tm{};
    std::istringstream stream(value);
    stream >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (stream.fail()) {
        return std::nullopt;
    }

    const std::time_t tt = std::mktime(&tm);
    if (tt == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(tt);
}

bool IsProcessActive(std::uint32_t pid) {
    if (pid == 0u) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return false;
    }
    DWORD exit_code = 0u;
    const bool active =
        GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return active;
}

std::filesystem::path RuntimeStatusPath(
    const std::filesystem::path& runtime_home) {
    return runtime_home / "control_runtime.json";
}

std::uint32_t CountDegradedChannels(const nlohmann::json& status) {
    const auto channels = status.find("controlled_channels");
    if (channels == status.end() || !channels->is_array()) {
        return 0u;
    }

    std::uint32_t count = 0u;
    for (const auto& channel : *channels) {
        if (!channel.is_object()) {
            continue;
        }
        const bool circuit_breaker_open =
            JsonBoolOr(channel, "circuit_breaker_open");
        const bool sensor_failed = JsonBoolOr(channel, "sensor_failed");
        const std::uint32_t write_failures =
            JsonUInt32Or(channel, "consecutive_write_failures");
        if (circuit_breaker_open || sensor_failed || write_failures > 0u) {
            ++count;
        }
    }
    return count;
}

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
    TryWriteJsonFileAtomic(result.runtime_home / "control_health.json",
                           payload);
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

    std::error_code ec;
    result.status_file_present = std::filesystem::exists(result.status_path, ec);
    if (ec || !result.status_file_present) {
        SetState(&result, RuntimeHealthState::kStopped,
                 "runtime status file is not present");
        return result;
    }

    nlohmann::json status;
    try {
        status = ReadJsonFile(result.status_path, "runtime status");
    } catch (const std::exception& error) {
        result.status_json_valid = false;
        SetState(&result, RuntimeHealthState::kFailed, error.what());
        return result;
    }
    if (!status.is_object()) {
        result.status_json_valid = false;
        SetState(&result, RuntimeHealthState::kFailed,
                 "runtime status JSON is not an object");
        return result;
    }
    result.status_json_valid = true;

    result.mode = JsonStringOr(status, "mode", "(unknown)");
    result.status = JsonStringOr(status, "status", "(unknown)");
    result.status_detail = JsonStringOr(status, "status_detail");
    result.last_update = JsonStringOr(
        status, "loop_last_evaluation", JsonStringOr(status, "last_refresh"));
    result.process_id = JsonUInt32Or(status, "process_id");
    result.process_active = IsProcessActive(result.process_id);
    result.degraded_channel_count = CountDegradedChannels(status);
    result.last_successful_restore_time =
        JsonStringOr(status, "last_successful_restore_time");

    if (result.pending_writes_unreadable) {
        SetState(&result, RuntimeHealthState::kFailed,
                 "pending-writes sidecar is unreadable");
        return result;
    }

    if (result.status == "failed" || result.status == "direct-read-failed") {
        const std::string suffix =
            result.status_detail.empty() ? "" : ": " + result.status_detail;
        SetState(&result, RuntimeHealthState::kFailed,
                 "runtime status is " + result.status + suffix);
        return result;
    }

    if (result.status == "shutdown") {
        SetState(&result, RuntimeHealthState::kStopped,
                 "runtime reported shutdown");
        return result;
    }

    if (!result.process_active) {
        SetState(&result, RuntimeHealthState::kStopped,
                 "runtime process is not active");
        return result;
    }

    if (JsonBoolOr(status, "stale")) {
        SetState(&result, RuntimeHealthState::kStale,
                 "runtime status reports stale telemetry");
        return result;
    }

    const auto parsed_last_update = ParseLocalIso8601(result.last_update);
    if (!parsed_last_update.has_value()) {
        SetState(&result, RuntimeHealthState::kStale,
                 "runtime status has no parseable last update timestamp");
        return result;
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
        return result;
    }

    if (result.stop_request_present) {
        SetState(&result, RuntimeHealthState::kDegraded,
                 "stop request is present while runtime process is active");
        return result;
    }

    if (result.degraded_channel_count > 0u) {
        SetState(&result, RuntimeHealthState::kDegraded,
                 "one or more controlled channels report degraded state");
        return result;
    }

    if (result.status != "running") {
        SetState(&result, RuntimeHealthState::kDegraded,
                 "runtime status is not running");
        return result;
    }

    SetState(&result, RuntimeHealthState::kHealthy,
             "runtime process is active and status is fresh");
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

}  // namespace svg_mb_control
