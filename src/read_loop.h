#pragma once

#include "control_config.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace svg_mb_control {

// Long-running read-only supervisor. It samples AMD, GPU, and fan telemetry
// in-process and republishes a Control-owned current_state.json plus
// control_runtime.json into the runtime home.
class ReadLoop {
  public:
    struct Status {
        std::string status;              // "running" | "shutdown" | "direct-read-failed"
        std::string status_detail;
        std::string last_refresh_iso;    // empty until the first successful parse
        std::string snapshot_source;
        std::uint32_t restart_count = 0u;
        std::uint64_t skipped_polls = 0u;
        std::uint64_t successful_polls = 0u;
        bool stale = true;
        std::uint32_t child_pid = 0u;
    };

    ReadLoop(ControlConfig config, std::filesystem::path runtime_home);
    ~ReadLoop();

    ReadLoop(const ReadLoop&) = delete;
    ReadLoop& operator=(const ReadLoop&) = delete;

    // Runs until RequestStop() is called. Returns 0 on clean shutdown,
    // non-zero on terminal direct reader failure.
    int RunUntilStopped();

    // Thread-safe. Signals the run loop to stop cooperatively.
    void RequestStop();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::filesystem::path ResolveRuntimeHomePath(const ControlConfig& config);

}  // namespace svg_mb_control
