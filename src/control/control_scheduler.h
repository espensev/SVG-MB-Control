#pragma once

#include "control_runtime_context.h"
#include "runtime_csv_rows.h"
#include "runtime_util.h"  // FormatLocalIso8601 (re-exported for callers)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace svg_mb_control {

double DurationMilliseconds(std::chrono::steady_clock::duration duration);

struct ProcessResourceSample {
    bool valid_cpu = false;
    bool valid_memory = false;
    std::uint64_t total_cpu_100ns = 0u;
    std::uint64_t working_set_bytes = 0u;
    std::uint64_t private_bytes = 0u;
    // Whole-system CPU time counters from GetSystemTimes, in 100 ns units
    // summed across all logical processors. kernel_100ns includes idle_100ns.
    bool valid_system_cpu = false;
    std::uint64_t system_idle_100ns = 0u;
    std::uint64_t system_kernel_100ns = 0u;
    std::uint64_t system_user_100ns = 0u;
    std::chrono::steady_clock::time_point sampled_at =
        std::chrono::steady_clock::time_point{};
};

class TimerResolutionScope {
  public:
    explicit TimerResolutionScope(unsigned int period_ms);
    ~TimerResolutionScope();

    TimerResolutionScope(const TimerResolutionScope&) = delete;
    TimerResolutionScope& operator=(const TimerResolutionScope&) = delete;

    bool active() const;
    unsigned int period_ms() const;

  private:
    unsigned int period_ms_ = 0u;
    bool active_ = false;
};

std::uint32_t ActiveProcessorCount();
ProcessResourceSample SampleProcessResources();

void UpdateTimingResources(RuntimeControlLoopTimingState* timing,
                           const ProcessResourceSample& previous,
                           const ProcessResourceSample& current,
                           bool have_previous,
                           std::uint32_t processor_count);

void WaitForNextControlTick(ControlRuntimeContext& context,
                            std::chrono::steady_clock::time_point tick_started,
                            std::uint32_t effective_interval_ms,
                            const std::atomic<bool>& stop_flag);

}  // namespace svg_mb_control
