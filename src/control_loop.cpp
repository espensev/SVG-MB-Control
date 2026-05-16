#include "control_loop.h"

#include "amd_reader.h"
#include "channel_evaluator.h"
#include "control_runtime_context.h"
#include "control_scheduler.h"
#include "control_status_writer.h"
#include "direct_runtime_snapshot.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "pending_writes.h"
#include "runtime_artifacts.h"
#include "runtime_logging.h"
#include "runtime_lifecycle.h"
#include "runtime_snapshot.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace svg_mb_control {

// Configuration parsing/validation (LoadControlLoopConfig and its validators)
// lives in control_loop_config.cpp. This translation unit is the steady-state
// control loop engine only.

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

int ControlLoop::RunUntilStopped(const std::atomic<bool>& stop_flag) {
    ControlRuntimeContext& context = impl_->context;
    ClearRuntimeStopRequest(context.runtime_home);
    const std::string event_log_path =
        ResolveRuntimeEventLogPath(context.runtime_home).string();
    RuntimeCsvLogger csv_logger(
        context.runtime_home,
        context.base.log_rotate_hours,
        context.base.log_retain_days);
    std::string log_csv_path;
    std::string log_manifest_path;
    if (csv_logger.Open("control-loop", BuildControlLoopCsvHeader())) {
        log_csv_path = csv_logger.active_archive_path().string();
        log_manifest_path = csv_logger.manifest_path().string();
    }
    RuntimeControlLoopTimingState last_timing;
    last_timing.loop_intended_interval_ms = context.loop.poll_tick_ms;

    AmdReader amd_reader;
    GpuReader gpu_reader;

    std::unique_ptr<FanWriter> fan_writer;
    try {
        fan_writer = CreateFanWriter(context.runtime_policy);
    } catch (const std::exception& error) {
        AppendRuntimeEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .mode = "control-loop",
                .event_type = "control_loop.init_failed",
                .detail = std::string("direct writer init failed: ") +
                          error.what(),
                .success = false,
            });
        WriteControlLoopStatus(context.runtime_home, "control-loop", "failed",
                        std::string("direct writer init failed: ") + error.what(),
                        0u, FormatLocalIso8601(std::chrono::system_clock::now()),
                        last_timing, context.channels, log_csv_path,
                        log_manifest_path, event_log_path);
        return 1;
    }

    TimerResolutionScope timer_resolution(1u);

    std::uint64_t tick_count = 0u;
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
                        detail.str(), tick_count,
                        FormatLocalIso8601(std::chrono::system_clock::now()),
                        last_timing, context.channels, log_csv_path,
                        log_manifest_path, event_log_path);
    }
    AppendRuntimeEvent(
        context.runtime_home,
        RuntimeLogEvent{
            .mode = "control-loop",
            .event_type = "control_loop.start",
            .detail = "control-loop started",
            .success = true,
        });

    bool fatal_restore_timeout = false;
    std::uint32_t fatal_restore_channel = 0u;
    std::chrono::steady_clock::time_point previous_tick_start;
    bool have_previous_tick_start = false;
    // Throttle snapshot-file writes: per-tick atomic rename is expensive on
    // Windows and the per-tick CSV row already records the same state. The
    // read-loop process publishes snapshot.json at full poll rate; the
    // control loop only needs to keep it loosely fresh for status consumers.
    std::chrono::steady_clock::time_point last_snapshot_write_time;
    bool have_last_snapshot_write_time = false;
    constexpr std::chrono::milliseconds kSnapshotWriteMinInterval{1000};

    // In-memory pending-writes cache. Upserts still persist synchronously
    // (crash-recovery contract); removals are batched and flushed at the
    // end of each tick so we avoid an extra JSON parse + serialize +
    // atomic rename per channel per tick on the happy path.
    PendingWritesStore pending_store(context.runtime_home);
    try {
        pending_store.Load();
    } catch (const std::exception& e) {
        AppendRuntimeEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .mode = "control-loop",
                .event_type = "control_loop.sidecar_load_failed",
                .detail = std::string(
                              "failed to load pending-writes sidecar; "
                              "continuing with empty cache: ") +
                          e.what(),
                .success = false,
            });
    }
    const std::uint32_t processor_count = ActiveProcessorCount();
    ProcessResourceSample resource_window_sample = SampleProcessResources();
    bool have_resource_window_sample = resource_window_sample.valid_cpu ||
        resource_window_sample.valid_memory;
    double last_process_cpu_delta_ms =
        std::numeric_limits<double>::quiet_NaN();
    double last_process_cpu_pct = std::numeric_limits<double>::quiet_NaN();

    while (!stop_flag.load() &&
           !RuntimeStopRequested(context.runtime_home)) {
        ++tick_count;
        const auto tick_started_steady = std::chrono::steady_clock::now();
        const auto tick_started_wall = std::chrono::system_clock::now();
        double achieved_interval_ms =
            std::numeric_limits<double>::quiet_NaN();
        if (have_previous_tick_start) {
            achieved_interval_ms =
                DurationMilliseconds(tick_started_steady - previous_tick_start);
        }
        previous_tick_start = tick_started_steady;
        have_previous_tick_start = true;
        const auto now_steady = tick_started_steady;
        const auto eval_iso = FormatLocalIso8601(tick_started_wall);

        RuntimeSnapshot runtime_snapshot = SampleDirectRuntimeSnapshot(
            amd_reader, gpu_reader, *fan_writer, context.runtime_policy);
        const bool runtime_snapshot_available =
            RuntimeSnapshotHasTelemetry(runtime_snapshot);

        if (csv_logger.MaybeRotate()) {
            log_csv_path = csv_logger.active_archive_path().string();
            log_manifest_path = csv_logger.manifest_path().string();
            AppendRuntimeEvent(
                context.runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.log_rotated",
                    .detail = "telemetry log rotated",
                    .tick_count = tick_count,
                    .success = true,
                });
        }

        // Extract CPU temp.
        TempInputs temp_inputs;
        if (runtime_snapshot_available) {
            const double cpu_c = FindRuntimeAmdSensorTemperature(
                runtime_snapshot, context.loop.cpu_temp_label);
            if (!std::isnan(cpu_c)) {
                temp_inputs.cpu_c = cpu_c;
                temp_inputs.cpu_available = true;
                temp_inputs.cpu_label = context.loop.cpu_temp_label;
            }
        }
        if (runtime_snapshot.gpu.available) {
            temp_inputs.gpu_c = GpuControlEnvelopeC(runtime_snapshot.gpu);
            temp_inputs.gpu_available = true;
            temp_inputs.gpu_label = "gpu_envelope";
        }

        if (runtime_snapshot_available) {
            const bool snapshot_write_due =
                !have_last_snapshot_write_time ||
                (now_steady - last_snapshot_write_time) >=
                    kSnapshotWriteMinInterval;
            if (snapshot_write_due) {
                WriteRuntimeSnapshotFile(context.runtime_home,
                                         runtime_snapshot);
                last_snapshot_write_time = now_steady;
                have_last_snapshot_write_time = true;
            }
        }

        // Per-channel decisions.
        for (auto& channel : context.channels) {
            const ChannelTimingConfig channel_timing =
                BuildChannelTimingConfig(context.loop, channel, now_steady);

            if (!channel.baseline_captured) {
                if (const RuntimeFanSnapshot* fan = FindRuntimeFanChannel(
                        runtime_snapshot, channel.config.channel)) {
                    channel.baseline_duty_raw = fan->duty_raw;
                    channel.baseline_mode_raw = fan->mode_raw;
                    channel.baseline_captured = true;
                    AppendRuntimeEvent(
                        context.runtime_home,
                        RuntimeLogEvent{
                            .mode = "control-loop",
                            .event_type = "control_loop.baseline_captured",
                            .detail = "captured baseline for control channel",
                            .channel = channel.config.channel,
                            .tick_count = tick_count,
                            .success = true,
                        });
                }
            }

            if (channel.write_active &&
                channel_timing.effective_hold_ms > 0u &&
                now_steady >= channel.hold_deadline &&
                channel.baseline_captured) {
                const FanWriteResult restore_result =
                    fan_writer->RestoreSavedState(channel.config.channel,
                                                  channel.baseline_duty_raw,
                                                  channel.baseline_mode_raw,
                                                  context.base.restore_timeout_ms);
                if (restore_result) {
                    AppendRuntimeEvent(
                        context.runtime_home,
                        RuntimeLogEvent{
                            .mode = "control-loop",
                            .event_type = "control_loop.restore_applied",
                            .detail = "restored channel to captured baseline",
                            .channel = channel.config.channel,
                            .tick_count = tick_count,
                            .success = true,
                        });
                    channel.write_active = false;
                    channel.last_issued_pct =
                        std::numeric_limits<double>::quiet_NaN();
                    try {
                        pending_store.QueueRemove(channel.config.channel);
                    } catch (const std::exception& e) {
                        // Best-effort; log but don't fail
                        AppendRuntimeEvent(
                            context.runtime_home,
                            RuntimeLogEvent{
                                .mode = "control-loop",
                                .event_type = "control_loop.sidecar_remove_warning",
                                .detail = std::string("best-effort sidecar removal after restore failed: ") + e.what(),
                                .channel = channel.config.channel,
                                .tick_count = tick_count,
                                .success = false,
                            });
                    }
                } else {
                    AppendRuntimeEvent(
                        context.runtime_home,
                        RuntimeLogEvent{
                            .mode = "control-loop",
                            .event_type = "control_loop.restore_failed",
                            .detail = restore_result.detail,
                            .channel = channel.config.channel,
                            .tick_count = tick_count,
                            .success = false,
                        });
                    if (restore_result.error == FanWriteError::kTimedOut) {
                        fatal_restore_timeout = true;
                        fatal_restore_channel = channel.config.channel;
                        break;
                    }
                }
            }

            const ChannelEvaluation evaluation = EvaluateChannel(
                channel, context.loop, temp_inputs, runtime_snapshot, now_steady);
            if (evaluation.sensor_event ==
                ChannelSensorEvent::FailureDetected) {
                AppendRuntimeEvent(
                    context.runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
                        .event_type = "control_loop.sensor_failure_detected",
                        .detail = "sensor failure detected, entering safe mode (100% duty)",
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .success = false,
                    });
            } else if (evaluation.sensor_event ==
                       ChannelSensorEvent::Recovered) {
                AppendRuntimeEvent(
                    context.runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
                        .event_type = "control_loop.sensor_recovered",
                        .detail = "sensor recovered, resuming normal control",
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .observed_temp_c =
                            evaluation.sensor_event_observed_temp_c,
                        .success = true,
                    });
            }
            if (!evaluation.has_setpoint) {
                continue;
            }

            const double observed_temp_c = evaluation.observed_temp_c;
            const double setpoint = evaluation.setpoint_pct;

            if (!std::isnan(channel.last_issued_pct)) {
                const double delta = std::abs(setpoint - channel.last_issued_pct);
                if (!evaluation.authority_reassert &&
                    delta < evaluation.timing.effective_deadband_pct) {
                    continue;
                }
            }

            if (std::isnan(channel.last_issued_pct)) {
                // First write for this channel — allow immediately.
            } else if (evaluation.timing.elapsed_since_last_write_ms <
                       WriteCooldownForAuthorityReassert(
                           evaluation.timing,
                           evaluation.authority_reassert)) {
                continue;
            }

            // Ensure we have a baseline before recording the sidecar.
            if (!channel.baseline_captured) {
                continue;  // skip until snapshot provides baseline
            }

            if (const RuntimeFanSnapshot* fan = FindRuntimeFanChannel(
                    runtime_snapshot, channel.config.channel)) {
                if (!fan->effective_write_allowed) {
                    continue;
                }
            }

            // Skip write if circuit breaker is open
            if (channel.circuit_breaker_open) {
                continue;
            }

            // Record sidecar entry.
            PendingWriteEntry entry;
            entry.channel = channel.config.channel;
            entry.baseline_duty_raw = channel.baseline_duty_raw;
            entry.baseline_mode_raw = channel.baseline_mode_raw;
            entry.target_pct = setpoint;
            entry.requested_hold_ms = evaluation.timing.effective_hold_ms;
            entry.started_iso = eval_iso;
            entry.child_pid = 0u;
            try {
                pending_store.Upsert(entry);
            } catch (const std::exception& e) {
                AppendRuntimeEvent(
                    context.runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
                        .event_type = "control_loop.sidecar_upsert_failed",
                        .detail = std::string("failed to write sidecar entry before fan write: ") + e.what(),
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .success = false,
                    });
                continue;
            }

            const FanWriteResult write_result =
                fan_writer->ApplyDuty(channel.config.channel, setpoint);
            if (!write_result) {
                // Circuit breaker: track consecutive failures
                ++channel.consecutive_write_failures;
                if (channel.consecutive_write_failures >= ChannelState::kMaxConsecutiveFailures) {
                    if (!channel.circuit_breaker_open) {
                        channel.circuit_breaker_open = true;
                        AppendRuntimeEvent(
                            context.runtime_home,
                            RuntimeLogEvent{
                                .mode = "control-loop",
                                .event_type = "control_loop.circuit_breaker_opened",
                                .detail = "circuit breaker opened after repeated write failures",
                                .channel = channel.config.channel,
                                .tick_count = tick_count,
                                .observed_temp_c = observed_temp_c,
                                .setpoint_pct = setpoint,
                                .success = false,
                            });
                    }
                }

                if (write_result.error == FanWriteError::kPolicyRefused) {
                    try {
                        pending_store.QueueRemove(channel.config.channel);
                    } catch (const std::exception& e) {
                        // Best-effort; log but continue
                        AppendRuntimeEvent(
                            context.runtime_home,
                            RuntimeLogEvent{
                                .mode = "control-loop",
                                .event_type = "control_loop.sidecar_remove_warning",
                                .detail = std::string("best-effort sidecar removal after policy refusal failed: ") + e.what(),
                                .channel = channel.config.channel,
                                .tick_count = tick_count,
                                .success = false,
                            });
                    }
                }
                AppendRuntimeEvent(
                    context.runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
                        .event_type = "control_loop.write_failed",
                        .detail = write_result.detail,
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .observed_temp_c = observed_temp_c,
                        .setpoint_pct = setpoint,
                        .success = false,
                    });
                continue;
            }

            // Success: reset circuit breaker
            if (channel.consecutive_write_failures > 0 || channel.circuit_breaker_open) {
                if (channel.circuit_breaker_open) {
                    AppendRuntimeEvent(
                        context.runtime_home,
                        RuntimeLogEvent{
                            .mode = "control-loop",
                            .event_type = "control_loop.circuit_breaker_closed",
                            .detail = "circuit breaker closed after successful write",
                            .channel = channel.config.channel,
                            .tick_count = tick_count,
                            .observed_temp_c = observed_temp_c,
                            .setpoint_pct = setpoint,
                            .success = true,
                        });
                }
                channel.consecutive_write_failures = 0u;
                channel.circuit_breaker_open = false;
            }
            if (evaluation.authority_reassert) {
                AppendRuntimeEvent(
                    context.runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
                        .event_type = "control_loop.authority_reasserted",
                        .detail = evaluation.authority_detail,
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .observed_temp_c = observed_temp_c,
                        .setpoint_pct = setpoint,
                        .success = true,
                    });
            }
            channel.write_active = true;
            channel.hold_deadline =
                evaluation.timing.effective_hold_ms == 0u
                    ? std::chrono::steady_clock::time_point::max()
                    : now_steady + std::chrono::milliseconds(
                          evaluation.timing.effective_hold_ms);
            channel.last_issued_pct = setpoint;
            channel.last_write_time = now_steady;
            ++channel.total_writes;
            AppendRuntimeEvent(
                context.runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.write_applied",
                    .detail = "applied control-loop setpoint",
                    .channel = channel.config.channel,
                    .tick_count = tick_count,
                    .observed_temp_c = observed_temp_c,
                    .setpoint_pct = setpoint,
                    .success = true,
                });
        }

        // Flush any queued pending-write removals once per tick. Upserts
        // already persisted synchronously inside the loop.
        try {
            pending_store.Flush();
        } catch (const std::exception& e) {
            AppendRuntimeEvent(
                context.runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.sidecar_flush_failed",
                    .detail =
                        std::string("end-of-tick sidecar flush failed: ") +
                        e.what(),
                    .tick_count = tick_count,
                    .success = false,
                });
        }

        const auto tick_finished_steady = std::chrono::steady_clock::now();
        const auto tick_finished_wall = std::chrono::system_clock::now();
        const double work_duration_ms =
            DurationMilliseconds(tick_finished_steady - tick_started_steady);
        RuntimeControlLoopTimingState tick_timing;
        tick_timing.loop_started_wall_clock = eval_iso;
        tick_timing.loop_finished_wall_clock =
            FormatLocalIso8601(tick_finished_wall);
        tick_timing.loop_work_duration_ms = work_duration_ms;
        tick_timing.loop_intended_interval_ms = context.loop.poll_tick_ms;
        tick_timing.loop_achieved_interval_ms = achieved_interval_ms;
        tick_timing.loop_slip_ms = std::isnan(achieved_interval_ms)
            ? std::numeric_limits<double>::quiet_NaN()
            : achieved_interval_ms -
                  static_cast<double>(context.loop.poll_tick_ms);
        tick_timing.loop_overrun =
            work_duration_ms > static_cast<double>(context.loop.poll_tick_ms);
        const ProcessResourceSample current_resource_sample =
            SampleProcessResources();
        if (current_resource_sample.valid_memory) {
            tick_timing.process_working_set_bytes =
                current_resource_sample.working_set_bytes;
            tick_timing.process_private_bytes =
                current_resource_sample.private_bytes;
        }
        const double resource_window_ms =
            have_resource_window_sample
                ? DurationMilliseconds(current_resource_sample.sampled_at -
                                       resource_window_sample.sampled_at)
                : 0.0;
        if (resource_window_ms >= 1000.0) {
            RuntimeControlLoopTimingState resource_timing;
            UpdateTimingResources(&resource_timing,
                                  resource_window_sample,
                                  current_resource_sample,
                                  have_resource_window_sample,
                                  processor_count);
            last_process_cpu_delta_ms = resource_timing.process_cpu_delta_ms;
            last_process_cpu_pct = resource_timing.process_cpu_pct;
            resource_window_sample = current_resource_sample;
            have_resource_window_sample =
                current_resource_sample.valid_cpu ||
                current_resource_sample.valid_memory;
        }
        tick_timing.process_cpu_delta_ms = last_process_cpu_delta_ms;
        tick_timing.process_cpu_pct = last_process_cpu_pct;
        last_timing = tick_timing;

        if (fatal_restore_timeout) {
            AppendRuntimeEvent(
                context.runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.abort",
                    .detail = "restore timed out; aborting control-loop",
                    .channel = fatal_restore_channel,
                    .tick_count = tick_count,
                    .success = false,
                });
            break;
        }

        if (csv_logger.is_open()) {
            csv_logger.WriteRow(
                BuildControlLoopCsvRow(
                    runtime_snapshot,
                    tick_count,
                    last_timing,
                    BuildChannelLogStates(context.channels)));
        }

        // Rate-limit status file writes to reduce disk I/O. CSV remains per tick.
        // Write every 10 ticks (500 ms with the shipped 50 ms cadence).
        constexpr std::uint32_t kStatusUpdateIntervalTicks = 10u;
        const bool should_write_status = (tick_count % kStatusUpdateIntervalTicks) == 0u;

        if (should_write_status) {
            std::ostringstream td;
            td << "tick poll_tick_ms=" << context.loop.poll_tick_ms
               << " cooldown=" << context.loop.write_cooldown_ms
               << " deadband=" << context.loop.deadband_pct
               << " timer_resolution_ms="
               << (timer_resolution.active()
                       ? std::to_string(timer_resolution.period_ms())
                       : std::string("default"));
            WriteControlLoopStatus(context.runtime_home, "control-loop", "running",
                            td.str(), tick_count, eval_iso, last_timing,
                            context.channels, log_csv_path, log_manifest_path,
                            event_log_path);
        }

        WaitForNextControlTick(context, tick_started_steady, stop_flag);
    }

    // Shutdown: restore controlled channels back to their captured baseline.
    WriteControlLoopStatus(context.runtime_home, "control-loop", "shutdown",
                    "stop requested", tick_count,
                    FormatLocalIso8601(std::chrono::system_clock::now()),
                    last_timing, context.channels, log_csv_path,
                    log_manifest_path, event_log_path);
    AppendRuntimeEvent(
        context.runtime_home,
        RuntimeLogEvent{
            .mode = "control-loop",
            .event_type = "control_loop.shutdown_requested",
            .detail = "stop requested",
            .tick_count = tick_count,
            .success = true,
        });
    bool restore_failure = false;
    for (auto& channel : context.channels) {
        if (fatal_restore_timeout && channel.write_active) {
            restore_failure = true;
            AppendRuntimeEvent(
                context.runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.shutdown_restore_skipped",
                    .detail = "skipped shutdown restore after earlier restore timeout",
                    .channel = channel.config.channel,
                    .tick_count = tick_count,
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
                AppendRuntimeEvent(
                    context.runtime_home,
                    RuntimeLogEvent{
                        .mode = "control-loop",
                        .event_type = "control_loop.shutdown_restore_failed",
                        .detail = restore_result.detail,
                        .channel = channel.config.channel,
                        .tick_count = tick_count,
                        .success = false,
                    });
                continue;
            }
            AppendRuntimeEvent(
                context.runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.shutdown_restore_applied",
                    .detail = "restored channel during shutdown",
                    .channel = channel.config.channel,
                    .tick_count = tick_count,
                    .success = true,
                });
        }
        try {
            pending_store.QueueRemove(channel.config.channel);
            pending_store.Flush();
        } catch (const std::exception& e) {
            restore_failure = true;
            AppendRuntimeEvent(
                context.runtime_home,
                RuntimeLogEvent{
                    .mode = "control-loop",
                    .event_type = "control_loop.shutdown_sidecar_remove_failed",
                    .detail = std::string("sidecar removal during shutdown failed: ") + e.what(),
                    .channel = channel.config.channel,
                    .tick_count = tick_count,
                    .success = false,
                });
        }
    }
    WriteControlLoopStatus(context.runtime_home, "control-loop", "shutdown",
                    restore_failure ? "restore failed"
                                    : "channels restored",
                    tick_count,
                    FormatLocalIso8601(std::chrono::system_clock::now()),
                    last_timing, context.channels, log_csv_path,
                    log_manifest_path, event_log_path);
    AppendRuntimeEvent(
        context.runtime_home,
        RuntimeLogEvent{
            .mode = "control-loop",
            .event_type = "control_loop.shutdown",
            .detail = restore_failure ? "restore failed"
                                      : "channels restored",
            .tick_count = tick_count,
            .success = !restore_failure,
        });
    return restore_failure ? 1 : 0;
}

}  // namespace svg_mb_control
