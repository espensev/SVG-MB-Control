#include "tick_runner.h"

#include "amd_reader.h"
#include "channel_evaluator.h"
#include "channel_write.h"
#include "control_status_writer.h"
#include "direct_runtime_snapshot.h"
#include "fan_writer.h"
#include "gpu_reader.h"
#include "low_band_evidence.h"
#include "low_band_integrator.h"
#include "pending_writes.h"
#include "runtime_artifacts.h"
#include "runtime_event_log.h"
#include "runtime_lifecycle.h"

#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <sstream>
#include <string>

namespace svg_mb_control {

namespace {

bool ChannelMatchesResetRequest(
    const RuntimeBreakerResetRequest& request,
    const ChannelState& channel) {
    return !request.channel.has_value() ||
           *request.channel == channel.config.channel;
}

void ProcessCircuitBreakerResetRequest(ControlRuntimeContext& context,
                                       std::uint64_t tick_count,
                                       ControlLoopRunState& state) {
    const auto request =
        TakeRuntimeBreakerResetRequest(context.runtime_home);
    if (!request.has_value()) {
        return;
    }

    state.force_status_write = true;

    if (!request->parse_error.empty()) {
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .event_type =
                    "control_loop.circuit_breaker_reset_invalid",
                .severity = "warning",
                .error_code =
                    "CONTROL_LOOP_CIRCUIT_BREAKER_RESET_INVALID",
                .detail = request->parse_error,
                .tick_count = tick_count,
                .success = false,
            });
        return;
    }

    std::uint32_t matching_channels = 0u;
    std::uint32_t reset_channels = 0u;
    for (auto& channel : context.channels) {
        if (!ChannelMatchesResetRequest(*request, channel)) {
            continue;
        }
        ++matching_channels;
        const bool had_breaker_state =
            channel.circuit_breaker_open ||
            channel.consecutive_write_failures > 0u;
        if (!had_breaker_state) {
            continue;
        }

        channel.circuit_breaker_open = false;
        channel.consecutive_write_failures = 0u;
        channel.last_write_reason = "breaker_reset";
        ++reset_channels;
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .event_type = "control_loop.circuit_breaker_reset",
                .detail = "operator reset of channel circuit breaker",
                .channel = channel.config.channel,
                .tick_count = tick_count,
                .success = true,
            });
    }

    if (reset_channels > 0u) {
        return;
    }

    std::ostringstream detail;
    if (matching_channels == 0u && request->channel.has_value()) {
        detail << "operator reset requested for unknown channel "
               << *request->channel;
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .event_type = "control_loop.circuit_breaker_reset_noop",
                .severity = "warning",
                .error_code =
                    "CONTROL_LOOP_CIRCUIT_BREAKER_RESET_NOOP",
                .detail = detail.str(),
                .channel = *request->channel,
                .tick_count = tick_count,
                .success = true,
            });
        return;
    }

    detail << "operator reset requested but no matching breaker was open";
    if (request->channel.has_value()) {
        detail << " for channel " << *request->channel;
    }
    AppendControlLoopEvent(
        context.runtime_home,
        RuntimeLogEvent{
            .event_type = "control_loop.circuit_breaker_reset_noop",
            .detail = detail.str(),
            .channel = request->channel,
            .tick_count = tick_count,
            .success = true,
        });
}

}  // namespace

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
                    ControlLoopRunState& state) {
    ++state.tick_count;
    const auto tick_started_steady = std::chrono::steady_clock::now();
    const auto tick_started_wall = std::chrono::system_clock::now();
    double achieved_interval_ms =
        std::numeric_limits<double>::quiet_NaN();
    if (state.have_previous_tick_start) {
        achieved_interval_ms = DurationMilliseconds(
            tick_started_steady - state.previous_tick_start);
    }
    state.previous_tick_start = tick_started_steady;
    state.have_previous_tick_start = true;
    const auto now_steady = tick_started_steady;
    const auto eval_iso = FormatLocalIso8601(tick_started_wall);

    SampleDirectRuntimeSnapshot(amd_reader, gpu_reader, fan_writer,
                                context.runtime_policy,
                                state.runtime_snapshot);
    const bool runtime_snapshot_available =
        RuntimeSnapshotHasTelemetry(state.runtime_snapshot);

    if (csv_logger.MaybeRotate()) {
        state.log_csv_path = csv_logger.active_archive_path().string();
        state.log_manifest_path = csv_logger.manifest_path().string();
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .event_type = "control_loop.log_rotated",
                .detail = "telemetry log rotated",
                .tick_count = state.tick_count,
                .success = true,
            });
    }

    // Extract CPU temp.
    TempInputs temp_inputs;
    if (runtime_snapshot_available) {
        const double cpu_c = FindRuntimeAmdSensorTemperature(
            state.runtime_snapshot, context.loop.cpu_temp_label);
        if (!std::isnan(cpu_c)) {
            temp_inputs.cpu_c = cpu_c;
            temp_inputs.cpu_available = true;
            temp_inputs.cpu_label = context.loop.cpu_temp_label;
        }
    }
    if (state.runtime_snapshot.gpu.available) {
        temp_inputs.gpu_c = GpuControlEnvelopeC(state.runtime_snapshot.gpu);
        temp_inputs.gpu_available = true;
        temp_inputs.gpu_label = "gpu_envelope";
    }

    ProcessCircuitBreakerResetRequest(context, state.tick_count, state);

    std::uint64_t low_band_elapsed_ms = context.loop.poll_tick_ms;
    if (std::isfinite(achieved_interval_ms) &&
        achieved_interval_ms > 0.0) {
        low_band_elapsed_ms =
            static_cast<std::uint64_t>(achieved_interval_ms + 0.5);
    }
    UpdateLowBandState(context, temp_inputs, state.runtime_snapshot,
                       low_band_elapsed_ms, now_steady, state.tick_count);

    if (runtime_snapshot_available) {
        constexpr std::chrono::milliseconds kSnapshotWriteMinInterval{1000};
        const bool snapshot_write_due =
            !state.have_last_snapshot_write_time ||
            (now_steady - state.last_snapshot_write_time) >=
                kSnapshotWriteMinInterval;
        if (snapshot_write_due) {
            WriteRuntimeSnapshotFile(context.runtime_home,
                                     state.runtime_snapshot);
            state.last_snapshot_write_time = now_steady;
            state.have_last_snapshot_write_time = true;
        }
    }

    // Per-channel decisions.
    for (auto& channel : context.channels) {
        const ChannelTimingConfig channel_timing =
            BuildChannelTimingConfig(context.loop, channel, now_steady);

        CaptureChannelBaselineIfAvailable(
            context, channel, state.runtime_snapshot, state.tick_count);

        if (!HandleExpiredHoldRestore(context,
                                      channel,
                                      channel_timing,
                                      fan_writer,
                                      pending_store,
                                      now_steady,
                                      state.tick_count,
                                      state.last_successful_restore_iso,
                                      state.fatal_restore_timeout,
                                      state.fatal_restore_channel)) {
            break;
        }

        const ChannelEvaluation evaluation = EvaluateChannel(
            channel, context.loop, temp_inputs, state.runtime_snapshot,
            now_steady);
        AppendChannelSensorEvent(context, channel, evaluation,
                                 state.tick_count);
        TryApplyChannelSetpoint(context,
                                channel,
                                state.runtime_snapshot,
                                evaluation,
                                fan_writer,
                                pending_store,
                                eval_iso,
                                now_steady,
                                state.tick_count);
    }

    // Flush any queued pending-write removals once per tick. Upserts
    // already persisted synchronously inside the loop.
    try {
        pending_store.Flush();
    } catch (const std::exception& e) {
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .event_type = "control_loop.sidecar_flush_failed",
                .detail =
                    std::string("end-of-tick sidecar flush failed: ") +
                    e.what(),
                .tick_count = state.tick_count,
                .success = false,
            });
    }

    const auto tick_finished_steady = std::chrono::steady_clock::now();
    const auto tick_finished_wall = std::chrono::system_clock::now();
    const double work_duration_ms =
        DurationMilliseconds(tick_finished_steady - tick_started_steady);
    const CadenceTick cadence = ComputeCadence(
        context.loop, temp_inputs, achieved_interval_ms,
        state.cadence_state);
    RuntimeControlLoopTimingState tick_timing;
    tick_timing.loop_started_wall_clock = eval_iso;
    tick_timing.loop_finished_wall_clock =
        FormatLocalIso8601(tick_finished_wall);
    tick_timing.loop_work_duration_ms = work_duration_ms;
    tick_timing.loop_intended_interval_ms = cadence.effective_interval_ms;
    tick_timing.cadence_transient = cadence.transient;
    tick_timing.loop_achieved_interval_ms = achieved_interval_ms;
    tick_timing.loop_slip_ms = std::isnan(achieved_interval_ms)
        ? std::numeric_limits<double>::quiet_NaN()
        : achieved_interval_ms -
              static_cast<double>(context.loop.poll_tick_ms);
    tick_timing.loop_overrun =
        work_duration_ms > static_cast<double>(context.loop.poll_tick_ms);
    const bool resource_sample_due = !state.have_resource_window_sample ||
        DurationMilliseconds(tick_finished_steady -
                             state.resource_window_sample.sampled_at) >=
            1000.0;
    if (resource_sample_due) {
        const ProcessResourceSample current_resource_sample =
            SampleProcessResources();
        if (current_resource_sample.valid_memory) {
            state.last_process_working_set_bytes =
                current_resource_sample.working_set_bytes;
            state.last_process_private_bytes =
                current_resource_sample.private_bytes;
        }
        const double resource_window_ms =
            state.have_resource_window_sample
                ? DurationMilliseconds(current_resource_sample.sampled_at -
                                       state.resource_window_sample.sampled_at)
                : 0.0;
        if (resource_window_ms >= 1000.0) {
            RuntimeControlLoopTimingState resource_timing;
            UpdateTimingResources(&resource_timing,
                                  state.resource_window_sample,
                                  current_resource_sample,
                                  state.have_resource_window_sample,
                                  processor_count);
            state.last_process_cpu_delta_ms =
                resource_timing.process_cpu_delta_ms;
            state.last_process_cpu_pct = resource_timing.process_cpu_pct;
        }
        state.resource_window_sample = current_resource_sample;
        state.have_resource_window_sample =
            current_resource_sample.valid_cpu ||
            current_resource_sample.valid_memory;
    }
    tick_timing.process_cpu_delta_ms = state.last_process_cpu_delta_ms;
    tick_timing.process_cpu_pct = state.last_process_cpu_pct;
    tick_timing.process_working_set_bytes =
        state.last_process_working_set_bytes;
    tick_timing.process_private_bytes = state.last_process_private_bytes;
    state.last_timing = tick_timing;

    if (state.fatal_restore_timeout) {
        AppendControlLoopEvent(
            context.runtime_home,
            RuntimeLogEvent{
                .event_type = "control_loop.abort",
                .detail = "restore timed out; aborting control-loop",
                .channel = state.fatal_restore_channel,
                .tick_count = state.tick_count,
                .success = false,
            });
        return false;
    }

    if (csv_logger.is_open()) {
        BuildChannelLogStates(context.channels, state.channel_log_states);
        csv_logger.WriteRow(
            BuildControlLoopCsvRow(
                state.runtime_snapshot,
                state.tick_count,
                state.last_timing,
                state.channel_log_states));
    }

    if (context.loop.low_band.enabled) {
        const auto evidence_interval = std::chrono::milliseconds(
            context.loop.low_band.evidence_write_interval_ms);
        const bool evidence_due =
            !state.have_last_low_band_evidence_write_time ||
            (tick_finished_steady -
                state.last_low_band_evidence_write_time) >= evidence_interval;
        if (evidence_due) {
            WriteLowBandEvidenceFile(context, state.tick_count);
            state.last_low_band_evidence_write_time = tick_finished_steady;
            state.have_last_low_band_evidence_write_time = true;
        }
    }

    // Rate-limit status file writes to reduce disk I/O. CSV remains per tick.
    // Write every kStatusUpdateIntervalTicks ticks (= 10 x poll_tick_ms;
    // 2.5 s at the shipped 250 ms cadence).
    constexpr std::uint32_t kStatusUpdateIntervalTicks = 10u;
    const bool should_write_status =
        state.force_status_write ||
        (state.tick_count % kStatusUpdateIntervalTicks) == 0u;

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
                        td.str(), state.tick_count, eval_iso,
                        state.last_timing, context.channels,
                        state.log_csv_path, state.log_manifest_path,
                        event_log_path, state.last_successful_restore_iso);
        state.force_status_write = false;
    }

    WaitForNextControlTick(context, tick_started_steady,
                           cadence.effective_interval_ms, stop_flag);
    return true;
}

}  // namespace svg_mb_control
