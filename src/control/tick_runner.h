#pragma once

#include "cadence_score.h"
#include "control_runtime_context.h"
#include "control_scheduler.h"
#include "runtime_csv_rows.h"
#include "runtime_snapshot.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace svg_mb_control {

class AmdReader;
class GpuReader;
class FanWriter;
class PendingWritesStore;
class RuntimeCsvLogger;

// Per-loop transient state threaded across ticks by RunControlTick.
// Reused buffers (runtime_snapshot, channel_log_states) retain their
// capacity across ticks so steady-state sampling does not reallocate.
// All fields default to the same values RunUntilStopped used when the
// tick body lived inline.
struct ControlLoopRunState {
    std::uint64_t tick_count = 0u;

    bool fatal_restore_timeout = false;
    std::uint32_t fatal_restore_channel = 0u;
    std::string last_successful_restore_iso;

    std::chrono::steady_clock::time_point previous_tick_start;
    bool have_previous_tick_start = false;

    CadenceRuntimeState cadence_state;

    std::chrono::steady_clock::time_point last_snapshot_write_time;
    bool have_last_snapshot_write_time = false;
    std::chrono::steady_clock::time_point last_low_band_evidence_write_time;
    bool have_last_low_band_evidence_write_time = false;

    ProcessResourceSample resource_window_sample;
    bool have_resource_window_sample = false;
    std::uint64_t last_process_working_set_bytes = 0u;
    std::uint64_t last_process_private_bytes = 0u;
    double last_process_cpu_delta_ms =
        std::numeric_limits<double>::quiet_NaN();
    double last_process_cpu_pct =
        std::numeric_limits<double>::quiet_NaN();

    RuntimeControlLoopTimingState last_timing;

    // Reused per-tick buffers — capacity retained across ticks.
    RuntimeSnapshot runtime_snapshot;
    RuntimeSnapshotIndex runtime_snapshot_index;
    std::vector<RuntimeControlChannelLogState> channel_log_states;

    // Active CSV/manifest paths for the current log chunk. Updated when
    // the logger rotates.
    std::string log_csv_path;
    std::string log_manifest_path;
    bool force_status_write = false;
};

// Runs one iteration of the steady-state control loop. Returns false
// when the tick observed a fatal restore-timeout and the surrounding
// RunUntilStopped while-loop must break out (the
// `control_loop.abort` event has already been appended). Returns true
// for normal ticks, including those that observed a stop request
// during the end-of-tick wait. `state.tick_count` is incremented
// before any sampling.
bool RunControlTick(ControlRuntimeContext& context,
                    const std::atomic<bool>& stop_flag,
                    AmdReader& amd_reader,
                    GpuReader& gpu_reader,
                    FanWriter& fan_writer,
                    RuntimeCsvLogger& csv_logger,
                    PendingWritesStore& pending_store,
                    const TimerResolutionScope& timer_resolution,
                    std::uint32_t processor_count,
                    const std::string& event_log_path,
                    ControlLoopRunState& state);

}  // namespace svg_mb_control
