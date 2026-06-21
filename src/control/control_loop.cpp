#include "control_loop.h"

#include "amd_reader.h"
#include "channel_controller.h"
#include "control_runtime_context.h"
#include "control_scheduler.h"
#include "control_status_writer.h"
#include "runtime_status.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "low_band_evidence.h"
#include "pending_writes.h"
#include "runtime_artifacts.h"
#include "runtime_csv_rows.h"
#include "runtime_event_log.h"
#include "runtime_lifecycle.h"
#include "tick_runner.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace svg_mb_control {

// Configuration parsing/validation (LoadControlLoopConfig and its validators)
// lives in control_loop_config.cpp. Per-tick sampling/decision/write work
// lives in tick_runner.cpp. This translation unit is the public ControlLoop
// surface plus the startup/shutdown choreography around the per-tick body.

// ------------------------ ControlLoop Impl -----------------------------

struct ControlLoop::Impl {
    Impl(ControlConfig base_config,
         ControlLoopConfig loop_config,
         std::filesystem::path runtime_home_path)
        : context(std::move(base_config),
                  std::move(loop_config),
                  std::move(runtime_home_path)) {}

    ControlRuntimeContext context;
};

ControlLoop::ControlLoop(ControlConfig base_config,
                         ControlLoopConfig loop_config,
                         std::filesystem::path runtime_home)
    : impl_(std::make_unique<Impl>(std::move(base_config),
                                   std::move(loop_config),
                                   std::move(runtime_home))) {}

ControlLoop::~ControlLoop() = default;

void ControlLoop::RequestStop() {
    std::lock_guard<std::mutex> lock(impl_->context.wake_mutex);
    impl_->context.wake_cv.notify_all();
}

int ControlLoop::RunUntilStopped(const std::atomic<bool>& stop_flag) {
    ControlRuntimeContext& context = impl_->context;
    ClearRuntimeStopRequest(context.runtime_home);
    const std::string event_log_path =
        ResolveRuntimeEventLogPath(context.runtime_home).string();
    ConfigureRuntimeEventLogRetention(
        ResolveRuntimeEventLogPath(context.runtime_home),
        RuntimeEventLogOptions{
            .rotate_hours = context.base.log_rotate_hours,
            .retain_days = context.base.log_retain_days,
            .reduce_routine_write_applied = true,
        });
    RuntimeCsvLogger csv_logger(
        context.runtime_home,
        context.base.log_rotate_hours,
        context.base.log_retain_days,
        context.base.csv_flush_interval_rows,
        RuntimeArtifactNaming{},
        RuntimeCsvIdentity{
            .config_path = context.base.source_path,
            .runtime_policy_path = context.runtime_policy.source_path,
            .control_poll_tick_ms = context.loop.poll_tick_ms,
            .control_write_cooldown_ms = context.loop.write_cooldown_ms,
        });

    ControlLoopRunState state;
    state.last_timing.loop_intended_interval_ms = context.loop.poll_tick_ms;
    if (csv_logger.Open("control-loop", BuildControlLoopCsvHeader())) {
        state.log_csv_path = csv_logger.active_archive_path().string();
        state.log_manifest_path = csv_logger.manifest_path().string();
    }

    AmdReader amd_reader;
    GpuReader gpu_reader;

    std::unique_ptr<FanWriter> fan_writer;
    try {
        fan_writer = CreateFanWriter(context.runtime_policy);
    } catch (const std::exception& error) {
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                    .event_type = "control_loop.init_failed",
                .detail = std::string("direct writer init failed: ") +
                          error.what(),
                .success = false,
            });
        WriteControlLoopStatus(context.runtime_home, "control-loop", "failed",
                        std::string("direct writer init failed: ") + error.what(),
                        0u, FormatLocalIso8601(std::chrono::system_clock::now()),
                        state.last_timing, context.channels, state.log_csv_path,
                        state.log_manifest_path, event_log_path);
        return 1;
    }

    TimerResolutionScope timer_resolution(1u);

    {
        std::ostringstream detail;
        detail << "direct telemetry active; fan_writer="
               << fan_writer->BackendLabel()
               << " policy="
               << (context.runtime_policy.present
                       ? context.runtime_policy.source_path.string()
                       : std::string("(none)"))
               << " poll_tick_ms="
               << context.loop.poll_tick_ms
               << " cooldown_ms=" << context.loop.write_cooldown_ms
               << " deadband_pct=" << context.loop.deadband_pct
               << " hold_ms=" << context.loop.control_hold_ms
               << " timer_resolution_ms="
               << (timer_resolution.active()
                       ? std::to_string(timer_resolution.period_ms())
                       : std::string("default"))
               << " channels=" << context.channels.size() << ")";
        WriteControlLoopStatus(context.runtime_home, "control-loop", "running",
                        detail.str(), state.tick_count,
                        FormatLocalIso8601(std::chrono::system_clock::now()),
                        state.last_timing, context.channels, state.log_csv_path,
                        state.log_manifest_path, event_log_path,
                        state.last_successful_restore_iso,
                        context.base.profile_name,
                        context.base.profile_resolution_source);
    }
    AppendControlLoopEvent(
        context.runtime_home,
        RuntimeLogEvent{
            .event_type = "control_loop.start",
            .detail = "control-loop started",
            .success = true,
        });

    // FEAT-0003 (REQ-PROFILE-07 / decision D6): record each PID channel's law at
    // startup. A live PID write crosses the measurement gate, so it emits
    // control_loop.profile_applied; a shadow/dry-run PID (allow_live absent, or
    // downgraded for missing evidence or a missing slew cap) emits
    // control_loop.pid_shadow with the reason. The curve law announces nothing.
    for (std::size_t i = 0; i < context.controllers.size(); ++i) {
        const ControllerStartupInfo info = context.controllers[i]->StartupInfo();
        if (!info.is_pid) {
            continue;
        }
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .event_type = info.live ? "control_loop.profile_applied"
                                        : "control_loop.pid_shadow",
                .detail = info.detail,
                .channel = context.channels[i].config.channel,
                .success = true,
            });
    }

    // In-memory pending-writes cache. Upserts still persist synchronously
    // (crash-recovery contract); removals are batched and flushed at the
    // end of each tick so we avoid an extra JSON parse + serialize +
    // atomic rename per channel per tick on the happy path.
    PendingWritesStore pending_store(context.runtime_home);
    try {
        pending_store.Load();
    } catch (const std::exception& e) {
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                    .event_type = "control_loop.sidecar_load_failed",
                .detail = std::string(
                              "failed to load pending-writes sidecar; "
                              "continuing with empty cache: ") +
                          e.what(),
                .success = false,
            });
    }
    const std::uint32_t processor_count = ActiveProcessorCount();
    state.resource_window_sample = SampleProcessResources();
    state.have_resource_window_sample =
        state.resource_window_sample.valid_cpu ||
        state.resource_window_sample.valid_memory;
    if (state.resource_window_sample.valid_memory) {
        state.last_process_working_set_bytes =
            state.resource_window_sample.working_set_bytes;
        state.last_process_private_bytes =
            state.resource_window_sample.private_bytes;
    }

    while (!stop_flag.load() &&
           !RuntimeStopRequested(context.runtime_home) &&
           !RuntimeProfileCycleRequested(context.runtime_home)) {
        if (!RunControlTick(context, stop_flag, amd_reader, gpu_reader,
                            *fan_writer, csv_logger, pending_store,
                            timer_resolution, processor_count,
                            event_log_path, state)) {
            break;  // fatal restore-timeout; abort event already emitted
        }
    }
    // FEAT-0023: a profile-cycle signal exits the loop cleanly (not via the
    // fatal break above), so the shutdown restore below runs and returns fans to
    // the captured BIOS baseline before the supervisor respawns the new profile.

    // Shutdown: restore controlled channels back to their captured baseline.
    WriteControlLoopStatus(context.runtime_home, "control-loop", "shutdown",
                    "stop requested", state.tick_count,
                    FormatLocalIso8601(std::chrono::system_clock::now()),
                    state.last_timing, context.channels, state.log_csv_path,
                    state.log_manifest_path, event_log_path,
                    state.last_successful_restore_iso,
                    context.base.profile_name,
                    context.base.profile_resolution_source);
    AppendControlLoopEvent(
        context.runtime_home,
        RuntimeLogEvent{
            .event_type = "control_loop.shutdown_requested",
            .detail = "stop requested",
            .tick_count = state.tick_count,
            .success = true,
        });
    bool restore_failure = false;
    std::vector<std::uint32_t> queued_sidecar_removals;
    const auto append_sidecar_remove_failure =
        [&](std::uint32_t channel, const std::string& detail) {
            restore_failure = true;
            AppendControlLoopEvent(
                context.runtime_home,
                RuntimeLogEvent{
                            .event_type =
                        "control_loop.shutdown_sidecar_remove_failed",
                    .detail = detail,
                    .channel = channel,
                    .tick_count = state.tick_count,
                    .success = false,
                });
        };
    for (auto& channel : context.channels) {
        if (state.fatal_restore_timeout && channel.write_active) {
            restore_failure = true;
            AppendControlLoopEvent(
                context.runtime_home,
                RuntimeLogEvent{
                            .event_type = "control_loop.shutdown_restore_skipped",
                    .detail = "skipped shutdown restore after earlier restore timeout",
                    .channel = channel.config.channel,
                    .tick_count = state.tick_count,
                    .success = false,
                });
            continue;
        }
        if (channel.write_active && channel.baseline_captured) {
            const FanWriteResult restore_result =
                fan_writer->RestoreSavedState(channel.config.channel,
                                              channel.baseline_duty_raw,
                                              channel.baseline_mode_raw,
                                              context.base.restore_timeout_ms);
            if (!restore_result) {
                restore_failure = true;
                AppendControlLoopEvent(
                    context.runtime_home,
                    RuntimeLogEvent{
                                    .event_type = "control_loop.shutdown_restore_failed",
                        .detail = restore_result.detail,
                        .channel = channel.config.channel,
                        .tick_count = state.tick_count,
                        .success = false,
                    });
                continue;
            }
            state.last_successful_restore_iso =
                FormatLocalIso8601(std::chrono::system_clock::now());
            AppendControlLoopEvent(
                context.runtime_home,
                RuntimeLogEvent{
                            .event_type = "control_loop.shutdown_restore_applied",
                    .detail = "restored channel during shutdown",
                    .channel = channel.config.channel,
                    .tick_count = state.tick_count,
                    .success = true,
                });
        }
        try {
            pending_store.QueueRemove(channel.config.channel);
            queued_sidecar_removals.push_back(channel.config.channel);
        } catch (const std::exception& e) {
            append_sidecar_remove_failure(
                channel.config.channel,
                std::string("sidecar removal during shutdown failed: ") +
                    e.what());
        }
    }
    if (!queued_sidecar_removals.empty()) {
        try {
            pending_store.Flush();
        } catch (const std::exception& e) {
            for (const std::uint32_t channel : queued_sidecar_removals) {
                append_sidecar_remove_failure(
                    channel,
                    std::string(
                        "sidecar removal flush during shutdown failed: ") +
                        e.what());
            }
        }
    }
    WriteControlLoopStatus(context.runtime_home, "control-loop", "shutdown",
                    restore_failure ? "restore failed"
                                    : "channels restored",
                    state.tick_count,
                    FormatLocalIso8601(std::chrono::system_clock::now()),
                    state.last_timing, context.channels, state.log_csv_path,
                    state.log_manifest_path, event_log_path,
                    state.last_successful_restore_iso);
    if (context.loop.low_band.enabled) {
        WriteLowBandEvidenceFile(context, state.tick_count);
    }
    AppendControlLoopEvent(
        context.runtime_home,
        RuntimeLogEvent{
            .event_type = "control_loop.shutdown",
            .detail = restore_failure ? "restore failed"
                                      : "channels restored",
            .tick_count = state.tick_count,
            .success = !restore_failure,
        });
    return restore_failure ? 1 : 0;
}

}  // namespace svg_mb_control
