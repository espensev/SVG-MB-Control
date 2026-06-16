#include "worker_force_terminate.h"

#include "runtime_status.h"
#include "runtime_supervisor_state.h"

#include <sstream>

#ifdef _WIN32
#include "windows_lean.h"

#include <vector>
#endif

namespace svg_mb_control {

namespace {

RuntimeLogEvent FailureEvent(std::string detail) {
    RuntimeLogEvent event;
    event.event_type = "supervisor.force_terminate_failed";
    event.detail = std::move(detail);
    event.success = false;
    return event;
}

}  // namespace

ForceTerminateOutcome EscalateForceTerminate(const ForceTerminateInputs& inputs,
                                             ProcessTerminator& terminator,
                                             std::uint32_t confirm_timeout_ms) {
    ForceTerminateOutcome outcome;

    // Guard 0: we need a worker PID to act on.
    if (inputs.worker_pid == 0u) {
        outcome.events.push_back(FailureEvent(
            "no worker PID resolved from control_supervisor.json; cannot "
            "escalate"));
        return outcome;
    }

    // Guard 1: corroborate the supervisor sidecar's worker PID against
    // control_runtime.json's process_id. A disagreement means the PID is
    // uncorroborated (possible Windows PID reuse); refuse rather than terminate
    // a PID we cannot confirm is the worker.
    if (inputs.worker_pid_runtime.has_value() &&
        *inputs.worker_pid_runtime != inputs.worker_pid) {
        std::ostringstream detail;
        detail << "worker PID disagreement: control_supervisor.json="
               << inputs.worker_pid << " control_runtime.json="
               << *inputs.worker_pid_runtime
               << "; refusing to force-terminate an uncorroborated PID";
        outcome.events.push_back(FailureEvent(detail.str()));
        return outcome;
    }

    // Kill the worker first. The supervisor self-exits once its child dies with
    // the stop sentinel already set (D1).
    const ForceTerminateResult worker_result = terminator.TerminateIfImageMatches(
        inputs.worker_pid, inputs.expected_image_path, confirm_timeout_ms);
    bool worker_gone = false;
    switch (worker_result) {
        case ForceTerminateResult::kConfirmedGone: {
            worker_gone = true;
            std::ostringstream detail;
            detail << "force-terminated hung worker pid=" << inputs.worker_pid
                   << " after graceful stop timed out (stop_result="
                   << inputs.graceful_stop_result << ")";
            RuntimeLogEvent event;
            event.event_type = "supervisor.worker_force_terminated";
            event.detail = detail.str();
            event.success = true;
            outcome.events.push_back(std::move(event));
            break;
        }
        case ForceTerminateResult::kNoSuchProcess:
            // The worker already exited between detection and escalation; do
            // not fabricate a kill event, but it is gone for relaunch purposes.
            worker_gone = true;
            break;
        case ForceTerminateResult::kImageMismatch: {
            std::ostringstream detail;
            detail << "refused to force-terminate worker pid=" << inputs.worker_pid
                   << ": its live image is not the expected release executable "
                      "(possible PID reuse)";
            outcome.events.push_back(FailureEvent(detail.str()));
            return outcome;
        }
        case ForceTerminateResult::kTerminateFailed:
        case ForceTerminateResult::kConfirmTimeout: {
            std::ostringstream detail;
            detail << "force-terminate of worker pid=" << inputs.worker_pid
                   << (worker_result == ForceTerminateResult::kConfirmTimeout
                           ? " did not confirm exit within budget"
                           : " failed");
            detail << "; relaunch deferred to the next watchdog poll";
            outcome.events.push_back(FailureEvent(detail.str()));
            return outcome;
        }
    }

    // The worker is gone. Give the supervisor a bounded window to self-exit,
    // then force-kill it only if it is still alive.
    bool supervisor_gone = false;
    if (inputs.supervisor_pid == 0u) {
        supervisor_gone = true;
    } else if (terminator.WaitForExit(inputs.supervisor_pid,
                                      inputs.expected_image_path,
                                      confirm_timeout_ms)) {
        supervisor_gone = true;  // self-exited (the common case)
    } else {
        const ForceTerminateResult supervisor_result =
            terminator.TerminateIfImageMatches(inputs.supervisor_pid,
                                               inputs.expected_image_path,
                                               confirm_timeout_ms);
        switch (supervisor_result) {
            case ForceTerminateResult::kConfirmedGone: {
                supervisor_gone = true;
                std::ostringstream detail;
                detail << "force-terminated wedged supervisor pid="
                       << inputs.supervisor_pid
                       << " (did not self-exit after worker death)";
                RuntimeLogEvent event;
                event.event_type = "supervisor.supervisor_force_terminated";
                event.detail = detail.str();
                event.success = true;
                outcome.events.push_back(std::move(event));
                break;
            }
            case ForceTerminateResult::kNoSuchProcess:
            case ForceTerminateResult::kImageMismatch:
                // No longer (or never) our supervisor; its singleton mutex is
                // released, so relaunch is safe.
                supervisor_gone = true;
                break;
            case ForceTerminateResult::kTerminateFailed:
            case ForceTerminateResult::kConfirmTimeout: {
                std::ostringstream detail;
                detail << "force-terminate of supervisor pid="
                       << inputs.supervisor_pid
                       << " did not confirm exit; relaunch deferred to the next "
                          "watchdog poll";
                outcome.events.push_back(FailureEvent(detail.str()));
                return outcome;  // runtime_gone stays false
            }
        }
    }

    outcome.runtime_gone = worker_gone && supervisor_gone;
    return outcome;
}

#ifdef _WIN32

namespace {

// Full image path of this running executable -- the worker and supervisor were
// both spawned from it (StartHiddenProcess(CurrentExecutablePath(), ...)), so a
// case-insensitive full-path match is the PID-reuse guard.
std::wstring CurrentExecutableImagePath() {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0u) {
            return {};
        }
        if (length < buffer.size() - 1u) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2u);
    }
}

bool ImagePathsEqual(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// Full image path of `process`, or empty on failure.
std::wstring ProcessImagePath(HANDLE process) {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        DWORD size = static_cast<DWORD>(buffer.size());
        if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
            return std::wstring(buffer.data(), size);
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
            buffer.size() >= 64u * 1024u) {
            return {};
        }
        buffer.resize(buffer.size() * 2u);
    }
}

bool ProcessConfirmedExited(HANDLE process) {
    DWORD exit_code = 0u;
    return GetExitCodeProcess(process, &exit_code) && exit_code != STILL_ACTIVE;
}

// The real terminator: opens ONE handle per PID and uses it for the image-path
// guard, TerminateProcess, and the post-kill exit confirm, so kConfirmedGone
// means the process death was verified on the handle we already held -- a
// later re-OpenProcess could be defeated by PID reuse.
class Win32ProcessTerminator : public ProcessTerminator {
 public:
    ForceTerminateResult TerminateIfImageMatches(
        std::uint32_t pid, const std::wstring& expected_image_path,
        std::uint32_t confirm_timeout_ms) override {
        if (pid == 0u) {
            return ForceTerminateResult::kNoSuchProcess;
        }
        HANDLE process = OpenProcess(
            PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
            FALSE, pid);
        if (process == nullptr) {
            // Cannot open: already gone, or a reused PID we have no rights to.
            // Either way the original process is gone.
            return ForceTerminateResult::kNoSuchProcess;
        }

        const std::wstring image = ProcessImagePath(process);
        if (!ImagePathsEqual(image, expected_image_path)) {
            CloseHandle(process);
            return ForceTerminateResult::kImageMismatch;
        }

        ForceTerminateResult result;
        if (TerminateProcess(process, 1u)) {
            const DWORD wait =
                WaitForSingleObject(process, confirm_timeout_ms);
            result = (wait == WAIT_OBJECT_0)
                         ? ForceTerminateResult::kConfirmedGone
                         : ForceTerminateResult::kConfirmTimeout;
        } else if (ProcessConfirmedExited(process)) {
            // It exited on its own between the guard and TerminateProcess.
            result = ForceTerminateResult::kConfirmedGone;
        } else {
            result = ForceTerminateResult::kTerminateFailed;
        }
        CloseHandle(process);
        return result;
    }

    bool WaitForExit(std::uint32_t pid, const std::wstring& expected_image_path,
                     std::uint32_t timeout_ms) override {
        if (pid == 0u) {
            return true;
        }
        HANDLE process = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr) {
            return true;  // gone
        }
        const std::wstring image = ProcessImagePath(process);
        if (!ImagePathsEqual(image, expected_image_path)) {
            CloseHandle(process);
            return true;  // a reused PID -- our process is gone
        }
        const DWORD wait = WaitForSingleObject(process, timeout_ms);
        CloseHandle(process);
        return wait == WAIT_OBJECT_0;
    }
};

}  // namespace

bool EscalateForceTerminate(const std::filesystem::path& runtime_home,
                            std::int32_t graceful_stop_result) {
    const std::optional<SupervisorState> state =
        ReadSupervisorState(runtime_home);
    if (!state.has_value()) {
        AppendRuntimeEvent(
            runtime_home,
            FailureEvent("no control_supervisor.json; cannot resolve PIDs to "
                         "force-terminate"));
        return false;
    }

    const std::optional<RuntimeStatusSnapshot> status =
        ReadRuntimeStatus(runtime_home);

    ForceTerminateInputs inputs;
    inputs.worker_pid = state->last_worker_pid;
    if (status.has_value() && status->process_id != 0u) {
        inputs.worker_pid_runtime = status->process_id;
    }
    inputs.supervisor_pid = state->supervisor_pid;
    inputs.expected_image_path = CurrentExecutableImagePath();
    inputs.graceful_stop_result = graceful_stop_result;

    const std::string mode = status.has_value() ? status->mode : std::string();

    Win32ProcessTerminator terminator;
    constexpr std::uint32_t kConfirmTimeoutMs = 3000u;
    ForceTerminateOutcome outcome =
        EscalateForceTerminate(inputs, terminator, kConfirmTimeoutMs);

    for (RuntimeLogEvent& event : outcome.events) {
        if (event.mode.empty()) {
            event.mode = mode;
        }
        AppendRuntimeEvent(runtime_home, event);
    }
    return outcome.runtime_gone;
}

#else  // !_WIN32

bool EscalateForceTerminate(const std::filesystem::path&, std::int32_t) {
    return false;
}

#endif  // _WIN32

}  // namespace svg_mb_control
