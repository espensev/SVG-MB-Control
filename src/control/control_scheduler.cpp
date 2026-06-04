#include "control_scheduler.h"

#include "runtime_lifecycle.h"

#include "windows_lean.h"
#include <mmsystem.h>
#include <psapi.h>

#include <algorithm>
#include <mutex>

namespace svg_mb_control {

double DurationMilliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

TimerResolutionScope::TimerResolutionScope(unsigned int period_ms)
    : period_ms_(period_ms),
      active_(timeBeginPeriod(period_ms_) == TIMERR_NOERROR) {}

TimerResolutionScope::~TimerResolutionScope() {
    if (active_) {
        timeEndPeriod(period_ms_);
    }
}

bool TimerResolutionScope::active() const {
    return active_;
}

unsigned int TimerResolutionScope::period_ms() const {
    return period_ms_;
}

namespace {

std::uint64_t FileTimeToU64(const FILETIME& value) {
    ULARGE_INTEGER out{};
    out.LowPart = value.dwLowDateTime;
    out.HighPart = value.dwHighDateTime;
    return out.QuadPart;
}

}  // namespace

std::uint32_t ActiveProcessorCount() {
    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count > 0u ? static_cast<std::uint32_t>(count) : 1u;
}

ProcessResourceSample SampleProcessResources() {
    ProcessResourceSample sample;
    sample.sampled_at = std::chrono::steady_clock::now();

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        sample.valid_cpu = true;
        sample.total_cpu_100ns = FileTimeToU64(kernel) + FileTimeToU64(user);
    }

    FILETIME system_idle{};
    FILETIME system_kernel{};
    FILETIME system_user{};
    if (GetSystemTimes(&system_idle, &system_kernel, &system_user)) {
        sample.valid_system_cpu = true;
        sample.system_idle_100ns = FileTimeToU64(system_idle);
        sample.system_kernel_100ns = FileTimeToU64(system_kernel);
        sample.system_user_100ns = FileTimeToU64(system_user);
    }

    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters))) {
        sample.valid_memory = true;
        sample.working_set_bytes =
            static_cast<std::uint64_t>(counters.WorkingSetSize);
        sample.private_bytes =
            static_cast<std::uint64_t>(counters.PrivateUsage);
    }

    return sample;
}

void UpdateTimingResources(RuntimeControlLoopTimingState* timing,
                           const ProcessResourceSample& previous,
                           const ProcessResourceSample& current,
                           bool have_previous,
                           std::uint32_t processor_count) {
    if (timing == nullptr) {
        return;
    }

    if (current.valid_memory) {
        timing->process_working_set_bytes = current.working_set_bytes;
        timing->process_private_bytes = current.private_bytes;
    }

    // Whole-system CPU busy time, derived independently of the process-cost
    // path below. GetSystemTimes counters are already summed across all
    // logical processors and kernel includes idle, so system_cpu_busy_pct is
    // the ratio of two whole-machine aggregates and must NOT be divided by
    // processor_count (unlike process_cpu_pct, which normalizes single-process
    // time to whole-machine capacity). See
    // docs/cpu-settings-evidence-logger-decision-2026-06-04.md.
    if (have_previous && previous.valid_system_cpu && current.valid_system_cpu &&
        current.system_idle_100ns >= previous.system_idle_100ns &&
        current.system_kernel_100ns >= previous.system_kernel_100ns &&
        current.system_user_100ns >= previous.system_user_100ns) {
        const double idle_delta_100ns = static_cast<double>(
            current.system_idle_100ns - previous.system_idle_100ns);
        const double kernel_delta_100ns = static_cast<double>(
            current.system_kernel_100ns - previous.system_kernel_100ns);
        const double user_delta_100ns = static_cast<double>(
            current.system_user_100ns - previous.system_user_100ns);
        const double total_delta_100ns = kernel_delta_100ns + user_delta_100ns;
        if (total_delta_100ns > 0.0) {
            timing->system_cpu_idle_delta_ms = idle_delta_100ns / 10000.0;
            timing->system_cpu_kernel_delta_ms = kernel_delta_100ns / 10000.0;
            timing->system_cpu_user_delta_ms = user_delta_100ns / 10000.0;
            timing->system_cpu_processor_count =
                (std::max)(1u, processor_count);
            // kernel_delta includes idle_delta, so busy = total - idle.
            const double busy_delta_100ns = total_delta_100ns - idle_delta_100ns;
            timing->system_cpu_busy_pct =
                (busy_delta_100ns / total_delta_100ns) * 100.0;
        }
    }

    if (!have_previous || !previous.valid_cpu || !current.valid_cpu ||
        current.total_cpu_100ns < previous.total_cpu_100ns) {
        return;
    }

    const double elapsed_ms =
        DurationMilliseconds(current.sampled_at - previous.sampled_at);
    if (elapsed_ms <= 0.0) {
        return;
    }

    const double cpu_delta_ms =
        static_cast<double>(current.total_cpu_100ns -
                            previous.total_cpu_100ns) /
        10000.0;
    timing->process_cpu_delta_ms = cpu_delta_ms;
    timing->process_cpu_pct =
        (cpu_delta_ms /
         (elapsed_ms * static_cast<double>((std::max)(1u, processor_count)))) *
        100.0;
}

void WaitForNextControlTick(ControlRuntimeContext& context,
                            std::chrono::steady_clock::time_point tick_started,
                            std::uint32_t effective_interval_ms,
                            const std::atomic<bool>& stop_flag) {
    const auto next_tick_deadline =
        tick_started + std::chrono::milliseconds(effective_interval_ms);
    if (std::chrono::steady_clock::now() >= next_tick_deadline) {
        return;  // loop work consumed the tick interval; do not wait
    }

    // The interactive/console stop path wakes wake_cv immediately via
    // ControlLoop::RequestStop(), so stop_flag is observed with no delay.
    // The supervisor stop path only writes stop.request.json from another
    // process and cannot notify wake_cv, so wait in bounded slices: this
    // re-checks RuntimeStopRequested() within kStopPollSlice instead of only
    // once per poll_tick_ms. One std::filesystem::exists() per slice is
    // negligible against the per-tick hardware sampling cost.
    constexpr std::chrono::milliseconds kStopPollSlice{50};
    const auto stop_now = [&] {
        return stop_flag.load() ||
               RuntimeStopRequested(context.runtime_home);
    };

    std::unique_lock<std::mutex> lock(context.wake_mutex);
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_tick_deadline) {
            return;
        }
        const auto slice_deadline =
            (std::min)(next_tick_deadline, now + kStopPollSlice);
        if (context.wake_cv.wait_until(lock, slice_deadline, stop_now)) {
            return;  // stop observed: console/signal flag or stop.request.json
        }
    }
}

}  // namespace svg_mb_control
