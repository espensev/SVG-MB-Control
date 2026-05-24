#pragma once

// Cross-process singleton enforcement for supervisor/worker processes that
// share a runtime_home directory. Backed by a Windows named mutex whose name
// is derived from a SHA-256 of the canonical runtime_home path; the kernel
// releases the mutex automatically on process exit (graceful or abnormal),
// and WAIT_ABANDONED on a previous-owner crash still produces a clean
// acquisition for the next caller.

#include <filesystem>
#include <string>

namespace svg_mb_control {

enum class SingletonRole {
    kSupervisor,
    kWorker,
};

class RuntimeSingletonLock {
public:
    RuntimeSingletonLock() = default;
    ~RuntimeSingletonLock();

    RuntimeSingletonLock(const RuntimeSingletonLock&) = delete;
    RuntimeSingletonLock& operator=(const RuntimeSingletonLock&) = delete;

    RuntimeSingletonLock(RuntimeSingletonLock&& other) noexcept;
    RuntimeSingletonLock& operator=(RuntimeSingletonLock&& other) noexcept;

    bool held() const noexcept { return handle_ != nullptr; }

    void Release() noexcept;

    // Adopt a Win32 HANDLE that has been acquired by WaitForSingleObject. The
    // lock takes ownership and will ReleaseMutex/CloseHandle on destruction.
    void Adopt(void* handle) noexcept;

private:
    void* handle_ = nullptr;
};

struct SingletonAcquisition {
    RuntimeSingletonLock lock;
    bool acquired = false;
    bool other_holder_present = false;
    std::string diagnostic;
};

// Best-effort non-blocking acquisition. Never throws.
//   - acquired=true: lock owns the mutex; caller keeps the lock for the
//     duration the singleton must be enforced.
//   - acquired=false, other_holder_present=true: another process holds the
//     singleton. diagnostic is human-readable.
//   - acquired=false, other_holder_present=false: open/wait failed for a
//     reason other than contention (rare). diagnostic explains what.
SingletonAcquisition TryAcquireRuntimeSingleton(
    SingletonRole role,
    const std::filesystem::path& runtime_home);

// Probe whether a singleton appears to be held by another process. Does not
// retain ownership; used by the launcher to print a friendly "already
// running" message before spawning a child that would otherwise fail to
// acquire on its own. Returns true only when the named mutex exists and is
// currently owned by some thread.
bool ProbeRuntimeSingletonHeld(SingletonRole role,
                               const std::filesystem::path& runtime_home);

}  // namespace svg_mb_control
