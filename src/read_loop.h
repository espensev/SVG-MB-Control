#pragma once

#include "control_config.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace svg_mb_control {

// Long-running read-only supervisor. Keeps one logger-service child alive,
// polls current_state.json at the configured cadence, writes Control-owned
// status to the runtime home, and tolerates non-atomic Bench publishes
// through a bounded retry loop.
class ReadLoop {
  public:
    struct Status {
        std::string status;              // "running" | "shutdown" | "child-died"
        std::string status_detail;
        std::string last_refresh_iso;    // empty until the first successful parse
        std::string snapshot_source;
        std::uint32_t restart_count = 0u;
        std::uint64_t skipped_polls = 0u;
        std::uint64_t successful_polls = 0u;
        bool stale = true;
        std::uint32_t child_pid = 0u;
    };

    ReadLoop(ControlConfig config,
             std::filesystem::path runtime_home,
             std::wstring bench_exe_path);
    ~ReadLoop();

    ReadLoop(const ReadLoop&) = delete;
    ReadLoop& operator=(const ReadLoop&) = delete;

    // Runs until RequestStop() is called or the restart budget is exhausted.
    // Returns 0 on clean shutdown, non-zero on terminal child failure.
    int RunUntilStopped();

    // Thread-safe. Signals the run loop to stop cooperatively.
    void RequestStop();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::filesystem::path ResolveRuntimeHomePath(const ControlConfig& config);

}  // namespace svg_mb_control
