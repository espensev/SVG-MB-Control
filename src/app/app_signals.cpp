#include "app/app_signals.h"

#include "control_loop.h"
#include "read_loop.h"
#include "windows_lean.h"

#include <atomic>
#include <stdexcept>

namespace svg_mb_control {

namespace {

std::atomic<ReadLoop*> g_active_read_loop{nullptr};
std::atomic<ControlLoop*> g_active_control_loop{nullptr};
std::atomic<bool> g_stop_signaled{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_stop_signaled.store(true);
            if (auto* read_loop = g_active_read_loop.load()) {
                read_loop->RequestStop();
            }
            if (auto* control_loop = g_active_control_loop.load()) {
                control_loop->RequestStop();
            }
            return TRUE;
        default:
            return FALSE;
    }
}

}  // namespace

ConsoleCtrlScope::~ConsoleCtrlScope() {
    Reset();
}

void ConsoleCtrlScope::Install() {
    if (installed_) {
        return;
    }
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        throw std::runtime_error("SetConsoleCtrlHandler failed.");
    }
    installed_ = true;
}

void ConsoleCtrlScope::Reset() noexcept {
    if (installed_) {
        SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
        installed_ = false;
    }
}

ActiveControlLoopScope::ActiveControlLoopScope(ControlLoop& control_loop) {
    g_active_control_loop.store(&control_loop);
    try {
        console_ctrl_.Install();
    } catch (...) {
        g_active_control_loop.store(nullptr);
        throw;
    }
}

ActiveControlLoopScope::~ActiveControlLoopScope() {
    console_ctrl_.Reset();
    g_active_control_loop.store(nullptr);
}

ActiveReadLoopScope::ActiveReadLoopScope(ReadLoop& read_loop) {
    g_active_read_loop.store(&read_loop);
    try {
        console_ctrl_.Install();
    } catch (...) {
        g_active_read_loop.store(nullptr);
        throw;
    }
}

ActiveReadLoopScope::~ActiveReadLoopScope() {
    console_ctrl_.Reset();
    g_active_read_loop.store(nullptr);
}

const std::atomic<bool>& StopSignaled() {
    return g_stop_signaled;
}

}  // namespace svg_mb_control
