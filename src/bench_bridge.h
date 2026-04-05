#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace svg_mb_control {

struct BridgeProcessResult {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
};

struct JsonArtifactLaunchResult {
    BridgeProcessResult process;
    std::filesystem::path json_artifact_path;
};

std::filesystem::path ResolveDefaultBenchExecutablePath();

BridgeProcessResult RunBenchProcess(
    const std::wstring& bench_exe_path,
    const std::vector<std::wstring>& args,
    std::uint32_t timeout_ms);

JsonArtifactLaunchResult RunReadSnapshot(
    const std::wstring& bench_exe_path,
    std::uint32_t timeout_ms);

JsonArtifactLaunchResult RunLoggerService(
    const std::wstring& bench_exe_path,
    std::uint32_t duration_ms,
    std::uint32_t timeout_ms);

std::string LoadJsonObjectFile(
    const std::filesystem::path& json_path);

// Long-running supervisor for a single Bench child process. Creates pipes,
// spawns the child with CREATE_NEW_PROCESS_GROUP, and drains stdout and stderr
// on background threads so the child does not block on a full pipe buffer.
//
// Lifetime: call Start() once. Destructor requests graceful stop and joins
// drain threads. Create a new supervisor instance to restart a child.
class BenchChildSupervisor {
  public:
    BenchChildSupervisor(std::wstring bench_exe_path,
                         std::vector<std::wstring> args);
    ~BenchChildSupervisor();

    BenchChildSupervisor(const BenchChildSupervisor&) = delete;
    BenchChildSupervisor& operator=(const BenchChildSupervisor&) = delete;

    // Spawns the child. Throws std::runtime_error on process creation failure.
    void Start();

    // Returns true while the child process is still active.
    bool IsRunning();

    // Exit code observed at child exit. Returns -1 while the child is still
    // running or if the child was never started.
    int LastExitCode();

    // Bounded tail buffers of captured output. Thread-safe to call.
    std::string StdoutTail() const;
    std::string StderrTail() const;

    // Sends CTRL_BREAK_EVENT to the child without waiting. Safe to call
    // multiple times. Callers that need to shut down multiple supervisors
    // should call this on each first, then call WaitForStop() on each, so
    // all children process their ctrl handlers concurrently.
    void SendStopSignal();

    // Waits up to graceful_timeout_ms for the child to exit after a
    // SendStopSignal() call. Falls back to TerminateProcess if the child
    // does not exit in time. Safe to call multiple times once the child
    // has exited.
    void WaitForStop(std::uint32_t graceful_timeout_ms);

    // Convenience: SendStopSignal() followed by WaitForStop(). Equivalent
    // to the previous single-call contract.
    void RequestStop(std::uint32_t graceful_timeout_ms);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace svg_mb_control
