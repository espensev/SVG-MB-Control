#pragma once

#include "control_config.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace svg_mb_control {

// Long-running mode the launcher/supervisor manages. Also used by the CLI
// dispatcher to select the in-process run path.
enum class RunMode {
    kOneShot,
    kReadLoop,
    kWriteOnce,
    kControlLoop,
    kCalibrate,
    kEvidenceLog,
};

// read-loop and control-loop are the supervised, long-running modes.
bool IsLongRunningMode(RunMode mode);

RunMode ParseRunMode(const wchar_t* value);
RunMode ParseRunMode(std::string_view value);

// Prints runtime status read from <runtime_home>/control_runtime.json.
// Returns a process exit code (currently always 0).
int PrintRuntimeStatus(const std::filesystem::path& runtime_home);

// Writes a stop request and waits up to 15s for the controller to report
// stopped. Returns 0 on success, 1 if the stop request cannot be written,
// 2 on a 15s timeout. quiet suppresses progress output (used by --restart).
int RequestStopAndWait(const std::filesystem::path& runtime_home,
                        bool quiet = false);

// Shows a yes/no confirmation dialog before a detached background launch.
// Returns true when the user chooses Yes.
bool ConfirmDetachedLaunch(RunMode mode, const ControlConfig& config);

// Launches a hidden background supervisor process for a long-running mode
// and returns once startup is confirmed. Returns 0 on success, otherwise the
// supervisor's startup exit code.
int LaunchDetachedLongRunningMode(RunMode mode, const ControlConfig& config);

// Runs the in-process supervisor loop: starts a worker, restarts it with
// backoff on unexpected non-zero exit, stops on stop-request. Returns 0 on
// clean shutdown, or the worker exit code on a startup failure.
int RunSupervisedLongRunningMode(RunMode mode, const ControlConfig& config);

}  // namespace svg_mb_control
