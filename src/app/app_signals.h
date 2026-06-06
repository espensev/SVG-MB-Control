#pragma once

#include <atomic>

namespace svg_mb_control {

class ControlLoop;
class ReadLoop;

// Registers the Win32 console control handler in Install() and removes
// it in Reset() / destruction. The handler sets a private stop flag
// that callers observe via StopSignaled(); see app_signals.cpp.
class ConsoleCtrlScope {
public:
    ConsoleCtrlScope() = default;
    ConsoleCtrlScope(const ConsoleCtrlScope&) = delete;
    ConsoleCtrlScope& operator=(const ConsoleCtrlScope&) = delete;
    ~ConsoleCtrlScope();

    void Install();
    void Reset() noexcept;

private:
    bool installed_ = false;
};

// Binds the active ControlLoop pointer for the console handler to call
// RequestStop() on Ctrl-C / shutdown, and installs the console scope.
// Construction must follow ControlLoop construction; destruction
// precedes ControlLoop destruction.
class ActiveControlLoopScope {
public:
    explicit ActiveControlLoopScope(ControlLoop& control_loop);
    ActiveControlLoopScope(const ActiveControlLoopScope&) = delete;
    ActiveControlLoopScope& operator=(const ActiveControlLoopScope&) = delete;
    ~ActiveControlLoopScope();

private:
    ConsoleCtrlScope console_ctrl_;
};

// Same shape as ActiveControlLoopScope but for ReadLoop.
class ActiveReadLoopScope {
public:
    explicit ActiveReadLoopScope(ReadLoop& read_loop);
    ActiveReadLoopScope(const ActiveReadLoopScope&) = delete;
    ActiveReadLoopScope& operator=(const ActiveReadLoopScope&) = delete;
    ~ActiveReadLoopScope();

private:
    ConsoleCtrlScope console_ctrl_;
};

// Const reference to the stop flag set by the Win32 console control
// handler. Pass to long-running modes (ReadLoop::RunUntilStopped,
// ControlLoop::RunUntilStopped, RunWriteOnce, RunCalibration,
// RunEvidenceLog) so they observe Ctrl-C / shutdown promptly.
const std::atomic<bool>& StopSignaled();

}  // namespace svg_mb_control
