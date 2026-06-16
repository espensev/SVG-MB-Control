#pragma once

// FEAT-0008 (REQ-WATCHDOG-*): bounded force-terminate escalation for a hung
// worker that the watchdog detects (kStale -> exit 2 -> --restart) but the
// graceful stop cannot clear. Lives behind an injectable ProcessTerminator seam
// so the escalation orchestration (worker-first ordering, the PID-reuse image
// guard, the worker-PID corroboration guard, the single-shot bound, and the
// runtime_gone relaunch gate) is unit-testable with a fake, matching the
// FanWriter/RecordingFanWriter idiom. The real Win32 adapter opens ONE handle
// per PID and uses it for the image-path guard, TerminateProcess, and the
// post-kill exit confirm, so a kConfirmedGone result means death verified on
// that handle (not a racy re-OpenProcess that PID reuse could defeat).
//
// Decision record: docs/watchdog-hung-worker-recovery-decision-2026-06-16.md
// (D1 worker-first, D2 reuse the 15 s graceful deadline, D3 bounded single-shot
// + image-path guard). Spec: docs/features/FEAT-0008-watchdog-hung-worker-recovery.md.

#include "runtime_event_log.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace svg_mb_control {

// Outcome of trying to force-terminate one process through a single OS handle.
enum class ForceTerminateResult {
    kNoSuchProcess,    // could not open the PID (already gone) -- treated as gone
    kImageMismatch,    // PID opened but its image path != expected -- refuse, NOT gone-by-us
    kConfirmedGone,    // terminated and exit confirmed within the budget on the handle
    kTerminateFailed,  // TerminateProcess itself failed
    kConfirmTimeout,   // terminated but exit not confirmed within the budget
};

// Injectable seam over the Win32 process operations. The real implementation is
// Win32ProcessTerminator; unit tests inject a fake that records calls and
// returns scripted results.
class ProcessTerminator {
 public:
    virtual ~ProcessTerminator() = default;

    // Open `pid`; if its full image path does not case-insensitively equal
    // `expected_image_path`, return kImageMismatch WITHOUT terminating.
    // Otherwise TerminateProcess and confirm exit within `confirm_timeout_ms`
    // on the same handle.
    virtual ForceTerminateResult TerminateIfImageMatches(
        std::uint32_t pid, const std::wstring& expected_image_path,
        std::uint32_t confirm_timeout_ms) = 0;

    // Wait up to `timeout_ms` for `pid` to exit. Returns true if the process is
    // gone (already exited, exits within the window, or its image no longer
    // matches expected_image_path -- i.e. not our process). Lets the supervisor
    // self-exit after the worker is killed (D1) before we escalate to killing
    // it too.
    virtual bool WaitForExit(std::uint32_t pid,
                             const std::wstring& expected_image_path,
                             std::uint32_t timeout_ms) = 0;
};

// Inputs the orchestration needs, resolved by the caller from the supervisor
// sidecar (control_supervisor.json) and control_runtime.json so the impl stays
// pure and testable.
struct ForceTerminateInputs {
    std::uint32_t worker_pid = 0u;                    // SupervisorState.last_worker_pid
    std::optional<std::uint32_t> worker_pid_runtime;  // control_runtime.json process_id (corroboration)
    std::uint32_t supervisor_pid = 0u;                // SupervisorState.supervisor_pid
    std::wstring expected_image_path;                 // CurrentExecutablePath() of the --restart invoker
    std::int32_t graceful_stop_result = 2;            // the RequestStopAndWait code that triggered escalation
};

struct ForceTerminateOutcome {
    // True only when both the worker and the supervisor are confirmed gone, so
    // the caller may relaunch without the singleton "already running" no-op trap.
    bool runtime_gone = false;
    // Events the caller must append to the runtime event log (PID + reason).
    std::vector<RuntimeLogEvent> events;
};

// Pure orchestration: bounded, single-shot. Kills the worker first (the
// supervisor self-exits once its child dies with the stop sentinel set), then
// force-kills the supervisor only if it has not exited within the confirm
// window. Enforces the worker-PID corroboration guard and relies on the
// terminator for the image-path guard. `confirm_timeout_ms` bounds each
// per-process wait so the whole escalation stays under the watchdog
// ExecutionTimeLimit. No retry loop.
ForceTerminateOutcome EscalateForceTerminate(const ForceTerminateInputs& inputs,
                                             ProcessTerminator& terminator,
                                             std::uint32_t confirm_timeout_ms);

// --restart entry point: reads the supervisor sidecar + control_runtime.json
// under `runtime_home`, builds inputs with the current executable path, runs
// EscalateForceTerminate with the Win32 terminator, appends the returned
// events, and returns whether the runtime is confirmed gone (relaunch is safe).
// `graceful_stop_result` is the RequestStopAndWait code that triggered the
// escalation, recorded in the event detail.
bool EscalateForceTerminate(const std::filesystem::path& runtime_home,
                            std::int32_t graceful_stop_result);

}  // namespace svg_mb_control
